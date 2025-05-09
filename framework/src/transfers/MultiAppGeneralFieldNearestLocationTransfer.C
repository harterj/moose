//* This file is part of the MOOSE framework
//* https://www.mooseframework.org
//*
//* All rights reserved, see COPYRIGHT for full restrictions
//* https://github.com/idaholab/moose/blob/master/COPYRIGHT
//*
//* Licensed under LGPL 2.1, please see LICENSE for details
//* https://www.gnu.org/licenses/lgpl-2.1.html

#include "MultiAppGeneralFieldNearestLocationTransfer.h"

// MOOSE includes
#include "FEProblem.h"
#include "MooseMesh.h"
#include "MooseTypes.h"
#include "MooseVariableFE.h"
#include "SystemBase.h"
#include "Positions.h"

#include "libmesh/system.h"

registerMooseObject("MooseApp", MultiAppGeneralFieldNearestLocationTransfer);
registerMooseObjectRenamed("MooseApp",
                           MultiAppGeneralFieldNearestNodeTransfer,
                           "12/31/2024 24:00",
                           MultiAppGeneralFieldNearestLocationTransfer);

InputParameters
MultiAppGeneralFieldNearestLocationTransfer::validParams()
{
  InputParameters params = MultiAppGeneralFieldTransfer::validParams();
  params.addClassDescription(
      "Transfers field data at the MultiApp position by finding the value at the nearest "
      "neighbor(s) in the origin application.");
  params.addParam<unsigned int>("num_nearest_points",
                                1,
                                "Number of nearest source (from) points will be chosen to "
                                "construct a value for the target point. All points will be "
                                "selected from the same origin mesh!");

  // Nearest node is historically more an extrapolation transfer
  params.set<Real>("extrapolation_constant") = GeneralFieldTransfer::BetterOutOfMeshValue;
  params.suppressParameter<Real>("extrapolation_constant");
  // We dont keep track of both point distance to app and to nearest node
  params.set<bool>("use_nearest_app") = false;
  params.suppressParameter<bool>("use_nearest_app");

  // the default of node/centroid switching based on the variable is causing lots of mistakes and
  // bad results
  std::vector<MooseEnum> source_types = {
      MooseEnum("nodes centroids variable_default", "variable_default")};
  params.addParam<std::vector<MooseEnum>>(
      "source_type", source_types, "Where to get the source values from for each source variable");

  return params;
}

MultiAppGeneralFieldNearestLocationTransfer::MultiAppGeneralFieldNearestLocationTransfer(
    const InputParameters & parameters)
  : MultiAppGeneralFieldTransfer(parameters),
    SolutionInvalidInterface(this),
    _num_nearest_points(getParam<unsigned int>("num_nearest_points"))
{
  if (_source_app_must_contain_point && _nearest_positions_obj)
    paramError("use_nearest_position",
               "We do not support using both nearest positions matching and checking if target "
               "points are within an app domain because the KDTrees for nearest-positions matching "
               "are (currently) built with data from multiple applications.");
}

void
MultiAppGeneralFieldNearestLocationTransfer::initialSetup()
{
  MultiAppGeneralFieldTransfer::initialSetup();

  // Handle the source types ahead of time
  const auto & source_types = getParam<std::vector<MooseEnum>>("source_type");
  _source_is_nodes.resize(_from_var_names.size());
  _use_zero_dof_for_value.resize(_from_var_names.size());
  if (source_types.size() != _from_var_names.size())
    mooseError("Not enough source types specified for this number of variables. Source types must "
               "be specified for transfers with multiple variables");
  for (const auto var_index : index_range(_from_var_names))
  {
    // Local app does not own any of the source problems
    if (_from_problems.empty())
      break;

    // Get some info on the source variable
    FEProblemBase & from_problem = *_from_problems[0];
    MooseVariableFieldBase & from_var =
        from_problem.getVariable(0,
                                 _from_var_names[var_index],
                                 Moose::VarKindType::VAR_ANY,
                                 Moose::VarFieldType::VAR_FIELD_ANY);
    System & from_sys = from_var.sys().system();
    const auto & fe_type = from_sys.variable_type(from_var.number());

    // Select where to get the origin values
    if (source_types[var_index] == "nodes")
      _source_is_nodes[var_index] = true;
    else if (source_types[var_index] == "centroids")
      _source_is_nodes[var_index] = false;
    else
    {
      // "Nodal" variables are continuous for sure at nodes
      if (from_var.isNodal())
        _source_is_nodes[var_index] = true;
      // for everyone else, we know they are continuous at centroids
      else
        _source_is_nodes[var_index] = false;
    }

    // Some variables can be sampled directly at their 0 dofs
    // - lagrange at nodes on a first order mesh
    // - anything constant and elemental obviously has the 0-dof value at the centroid (or
    // vertex-average). However, higher order elemental, even monomial, do not hold the centroid
    // value at dof index 0 For example: pyramid has dof 0 at the center of the base, prism has dof
    // 0 on an edge etc
    if ((_source_is_nodes[var_index] && fe_type.family == LAGRANGE &&
         !from_problem.mesh().hasSecondOrderElements()) ||
        (!_source_is_nodes[var_index] && fe_type.order == CONSTANT))
      _use_zero_dof_for_value[var_index] = true;
    else
      _use_zero_dof_for_value[var_index] = false;

    // Check with the source variable that it is not discontinuous at the source
    if (_source_is_nodes[var_index])
      if (from_var.getContinuity() == DISCONTINUOUS ||
          from_var.getContinuity() == SIDE_DISCONTINUOUS)
        mooseError("Source variable cannot be sampled at nodes as it is discontinuous");

    // Local app does not own any of the target problems
    if (_to_problems.empty())
      break;

    // Check with the target variable that we are not doing awful projections
    MooseVariableFieldBase & to_var =
        _to_problems[0]->getVariable(0,
                                     _to_var_names[var_index],
                                     Moose::VarKindType::VAR_ANY,
                                     Moose::VarFieldType::VAR_FIELD_ANY);
    System & to_sys = to_var.sys().system();
    const auto & to_fe_type = to_sys.variable_type(to_var.number());
    if (_source_is_nodes[var_index])
    {
      if (to_fe_type.order == CONSTANT)
        mooseWarning(
            "Transfer is projecting from nearest-nodes to centroids. This is likely causing "
            "floating point indetermination in the results because multiple nodes are 'nearest' to "
            "a centroid. Please consider using a ProjectionAux to build an elemental source "
            "variable (for example constant monomial) before transferring");
    }
    else if (to_var.isNodal())
      mooseWarning(
          "Transfer is projecting from nearest-centroids to nodes. This is likely causing "
          "floating point indetermination in the results because multiple centroids are "
          "'nearest' to a node. Please consider using a ProjectionAux to build a nodal source "
          "variable (for example linear Lagrange) before transferring");
  }
}

void
MultiAppGeneralFieldNearestLocationTransfer::prepareEvaluationOfInterpValues(
    const unsigned int var_index)
{
  _local_kdtrees.clear();
  _local_points.clear();
  _local_values.clear();
  buildKDTrees(var_index);
}

void
MultiAppGeneralFieldNearestLocationTransfer::buildKDTrees(const unsigned int var_index)
{
  const unsigned int num_sources =
      _nearest_positions_obj ? _nearest_positions_obj->getPositions(/*initial=*/false).size()
                             : _from_problems.size();

  _local_kdtrees.resize(num_sources);
  _local_points.resize(num_sources);
  _local_values.resize(num_sources);
  unsigned int max_leaf_size = 0;

  // Construct a local KDTree for each source (subapp or positions)
  for (unsigned int i_source = 0; i_source < num_sources; ++i_source)
  {
    // Nest a loop on apps only for the Positions case. Regular case only looks at one app
    const unsigned int num_apps_to_consider = _nearest_positions_obj ? _from_problems.size() : 1;
    for (const auto app_index : make_range(num_apps_to_consider))
    {
      const unsigned int i_from = !_nearest_positions_obj ? i_source : app_index;

      // NOTE: If we decide we do not want to mix apps when using the nearest-positions
      // we could continue here when the positions is not the nearest to the app

      FEProblemBase & from_problem = *_from_problems[i_from];
      auto & from_mesh = from_problem.mesh(_displaced_source_mesh);
      MooseVariableFieldBase & from_var =
          from_problem.getVariable(0,
                                   _from_var_names[var_index],
                                   Moose::VarKindType::VAR_ANY,
                                   Moose::VarFieldType::VAR_FIELD_ANY);

      System & from_sys = from_var.sys().system();
      const unsigned int from_var_num = from_sys.variable_number(getFromVarName(var_index));

      // Build KDTree from nodes and nodal values
      if (_source_is_nodes[var_index])
      {
        for (const auto & node : from_mesh.getMesh().local_node_ptr_range())
        {
          if (node->n_dofs(from_sys.number(), from_var_num) < 1)
            continue;

          if (!_from_blocks.empty() && !inBlocks(_from_blocks, from_mesh, node))
            continue;

          if (!_from_boundaries.empty() && !onBoundaries(_from_boundaries, from_mesh, node))
            continue;

          // Only add to the KDTree nodes that are closest to the 'position'
          // When querying values at a target point, the KDTree associated to the closest
          // position to the target point is queried
          if (_nearest_positions_obj &&
              !closestToPosition(i_source, *node + _from_positions[i_from]))
            continue;

          _local_points[i_source].push_back(*node + _from_positions[i_from]);
          auto dof = node->dof_number(from_sys.number(), from_var_num, 0);
          _local_values[i_source].push_back((*from_sys.solution)(dof));
          if (!_use_zero_dof_for_value[var_index])
            flagInvalidSolution(
                "Nearest-location is not implemented for this source variable type on "
                "this mesh. Returning value at dof 0");
        }
      }
      // Build KDTree from centroids and value at centroids
      else
      {
        for (auto & elem : as_range(from_mesh.getMesh().local_elements_begin(),
                                    from_mesh.getMesh().local_elements_end()))
        {
          if (elem->n_dofs(from_sys.number(), from_var_num) < 1)
            continue;

          if (!_from_blocks.empty() && !inBlocks(_from_blocks, elem))
            continue;

          if (!_from_boundaries.empty() && !onBoundaries(_from_boundaries, from_mesh, elem))
            continue;

          const auto vertex_average = elem->vertex_average();

          if (_nearest_positions_obj &&
              !closestToPosition(i_source, vertex_average + _from_positions[i_from]))
            continue;

          _local_points[i_source].push_back(vertex_average + _from_positions[i_from]);
          if (_use_zero_dof_for_value[var_index])
          {
            auto dof = elem->dof_number(from_sys.number(), from_var_num, 0);
            _local_values[i_source].push_back((*from_sys.solution)(dof));
          }
          else
            _local_values[i_source].push_back(
                from_sys.point_value(from_var_num, vertex_average, elem));
        }
      }
      max_leaf_size = std::max(max_leaf_size, from_mesh.getMaxLeafSize());
    }

    // Make a KDTree, from a single app in regular case, from all points from all apps nearest
    // a position in the nearest_position case
    std::shared_ptr<KDTree> _kd_tree =
        std::make_shared<KDTree>(_local_points[i_source], max_leaf_size);
    _local_kdtrees[i_source] = _kd_tree;
  }
}

void
MultiAppGeneralFieldNearestLocationTransfer::evaluateInterpValues(
    const std::vector<Point> & incoming_points, std::vector<std::pair<Real, Real>> & outgoing_vals)
{
  evaluateInterpValuesNearestNode(incoming_points, outgoing_vals);
}

void
MultiAppGeneralFieldNearestLocationTransfer::evaluateInterpValuesNearestNode(
    const std::vector<Point> & incoming_points, std::vector<std::pair<Real, Real>> & outgoing_vals)
{
  dof_id_type i_pt = 0;
  for (auto & pt : incoming_points)
  {
    // Reset distance
    outgoing_vals[i_pt].second = std::numeric_limits<Real>::max();
    bool point_found = false;

    const unsigned int num_sources =
        !_nearest_positions_obj ? _from_problems.size()
                                : _nearest_positions_obj->getPositions(/*initial=*/false).size();

    // Loop on all sources
    for (const auto i_from : make_range(num_sources))
    {
      // Only use the KDTree from the closest position if in "nearest-position" mode
      if (_nearest_positions_obj && !closestToPosition(i_from, pt))
        continue;

      std::vector<std::size_t> return_index(_num_nearest_points);
      std::vector<Real> return_dist_sqr(_num_nearest_points);

      // Check mesh restriction before anything
      if (_source_app_must_contain_point && !inMesh(_from_point_locators[i_from].get(), pt))
        continue;

      // KD Tree can be empty if no points are within block/boundary/bounding box restrictions
      if (_local_kdtrees[i_from]->numberCandidatePoints())
      {
        point_found = true;
        _local_kdtrees[i_from]->neighborSearch(
            pt, _num_nearest_points, return_index, return_dist_sqr);
        Real val_sum = 0, dist_sum = 0;
        for (auto index : return_index)
        {
          val_sum += _local_values[i_from][index];
          dist_sum += (_local_points[i_from][index] - pt).norm();
        }
        // Pick the closest
        if (dist_sum / return_dist_sqr.size() < outgoing_vals[i_pt].second)
          outgoing_vals[i_pt] = {val_sum / return_index.size(), dist_sum / return_dist_sqr.size()};
      }
    }

    // none of the source problem meshes were within the restrictions set
    if (!point_found)
      outgoing_vals[i_pt] = {GeneralFieldTransfer::BetterOutOfMeshValue,
                             GeneralFieldTransfer::BetterOutOfMeshValue};
    else if (_search_value_conflicts)
    {
      // Local source meshes conflicts can happen two ways:
      // - two (or more) problems' nearest nodes are equidistant to the target point
      // - within a problem, the num_nearest_points'th closest node is as close to the target
      //   point as the num_nearest_points + 1'th closest node
      unsigned int num_equidistant_problems = 0;

      for (const auto i_from : make_range(num_sources))
      {
        // Only use the KDTree from the closest position if in "nearest-position" mode
        if (_nearest_positions_obj && !closestToPosition(i_from, pt))
          continue;

        unsigned int num_search = _num_nearest_points + 1;
        std::vector<std::size_t> return_index(num_search);
        std::vector<Real> return_dist_sqr(num_search);

        if (_local_kdtrees[i_from]->numberCandidatePoints())
        {
          _local_kdtrees[i_from]->neighborSearch(pt, num_search, return_index, return_dist_sqr);
          auto num_found = return_dist_sqr.size();

          // Local coordinates only accessible when not using nearest-position
          Point local_pt = pt;
          if (!_nearest_positions_obj)
            local_pt -= _from_positions[i_from];

          // Look for too many equidistant nodes within a problem. First zip then sort by distance
          std::vector<std::pair<Real, std::size_t>> zipped_nearest_points;
          for (auto i : make_range(num_found))
            zipped_nearest_points.push_back(std::make_pair(return_dist_sqr[i], return_index[i]));
          std::sort(zipped_nearest_points.begin(), zipped_nearest_points.end());

          // If two furthest are equally far from target point, then we have an indetermination in
          // what is sent in this communication round from this process. However, it may not
          // materialize to an actual conflict, as values sent from another process for the desired
          // target point could be closer (nearest). There is no way to know at this point in the
          // communication that a closer value exists somewhere else
          if (num_found > 1 && num_found == num_search &&
              MooseUtils::absoluteFuzzyEqual(zipped_nearest_points[num_found - 1].first,
                                             zipped_nearest_points[num_found - 2].first))
            registerConflict(i_from, 0, local_pt, outgoing_vals[i_pt].second, true);

          // Recompute the distance for this problem. If it matches the cached value more than once
          // it means multiple problems provide equidistant values for this point
          Real dist_sum = 0;
          for (auto i : make_range(num_search - 1))
          {
            auto index = zipped_nearest_points[i].second;
            dist_sum += (_local_points[i_from][index] - pt).norm();
          }

          // Compare to the selected value found after looking at all the problems
          if (MooseUtils::absoluteFuzzyEqual(dist_sum / return_dist_sqr.size(),
                                             outgoing_vals[i_pt].second))
          {
            num_equidistant_problems++;
            if (num_equidistant_problems > 1)
              registerConflict(i_from, 0, local_pt, outgoing_vals[i_pt].second, true);
          }
        }
      }
    }

    // Move to next point
    i_pt++;
  }
}

bool
MultiAppGeneralFieldNearestLocationTransfer::inBlocks(const std::set<SubdomainID> & blocks,
                                                      const MooseMesh & mesh,
                                                      const Elem * elem) const
{
  // We need to override the definition of block restriction for an element
  // because we have to consider whether each node of an element is adjacent to a block
  for (const auto & i_node : make_range(elem->n_nodes()))
  {
    const auto & node = elem->node_ptr(i_node);
    const std::set<SubdomainID> & node_blocks = mesh.getNodeBlockIds(*node);
    std::set<SubdomainID> u;
    std::set_intersection(blocks.begin(),
                          blocks.end(),
                          node_blocks.begin(),
                          node_blocks.end(),
                          std::inserter(u, u.begin()));
    if (!u.empty())
      return true;
  }
  return false;
}
