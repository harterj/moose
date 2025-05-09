//* This file is part of the MOOSE framework
//* https://www.mooseframework.org
//*
//* All rights reserved, see COPYRIGHT for full restrictions
//* https://github.com/idaholab/moose/blob/master/COPYRIGHT
//*
//* Licensed under LGPL 2.1, please see LICENSE for details
//* https://www.gnu.org/licenses/lgpl-2.1.html

#include "PressureAction.h"
#include "Factory.h"
#include "FEProblem.h"
#include "Conversion.h"

registerMooseAction("TensorMechanicsApp", PressureAction, "add_bc");

InputParameters
PressureAction::validParams()
{
  InputParameters params = Action::validParams();
  params.addClassDescription("Set up Pressure boundary conditions");

  params.addRequiredParam<std::vector<BoundaryName>>(
      "boundary", "The list of boundary IDs from the mesh where the pressure will be applied");

  params.addRequiredParam<std::vector<VariableName>>(
      "displacements",
      "The displacements appropriate for the simulation geometry and coordinate system");

  params.addParam<std::vector<AuxVariableName>>("save_in_disp_x",
                                                "The save_in variables for x displacement");
  params.addParam<std::vector<AuxVariableName>>("save_in_disp_y",
                                                "The save_in variables for y displacement");
  params.addParam<std::vector<AuxVariableName>>("save_in_disp_z",
                                                "The save_in variables for z displacement");

  params.addParam<Real>("factor", 1.0, "The factor to use in computing the pressure");
  params.addParam<bool>("use_displaced_mesh", true, "Whether to use the displaced mesh.");
  params.addParam<Real>("hht_alpha",
                        0,
                        "alpha parameter for mass dependent numerical damping induced "
                        "by HHT time integration scheme");
  params.addDeprecatedParam<Real>(
      "alpha", "alpha parameter for HHT time integration", "Please use hht_alpha");
  params.addParam<FunctionName>("function", "The function that describes the pressure");
  params.addParam<bool>("use_automatic_differentiation",
                        false,
                        "Flag to use automatic differentiation (AD) objects when possible");
  params.addParam<std::vector<TagName>>("extra_vector_tags",
                                        "The extra tags for the vectors this Kernel should fill");
  params.addParam<std::vector<TagName>>(
      "absolute_value_vector_tags",
      "The tags for the vectors this residual object should fill with the "
      "absolute value of the residual contribution");
  return params;
}

PressureAction::PressureAction(const InputParameters & params)
  : Action(params), _use_ad(getParam<bool>("use_automatic_differentiation"))
{
  _save_in_vars.push_back(getParam<std::vector<AuxVariableName>>("save_in_disp_x"));
  _save_in_vars.push_back(getParam<std::vector<AuxVariableName>>("save_in_disp_y"));
  _save_in_vars.push_back(getParam<std::vector<AuxVariableName>>("save_in_disp_z"));

  _has_save_in_vars.push_back(params.isParamValid("save_in_disp_x"));
  _has_save_in_vars.push_back(params.isParamValid("save_in_disp_y"));
  _has_save_in_vars.push_back(params.isParamValid("save_in_disp_z"));
}

void
PressureAction::act()
{
  std::string ad_prepend = "";
  if (_use_ad)
    ad_prepend = "AD";

  std::string kernel_name = ad_prepend + "Pressure";

  std::vector<VariableName> displacements = getParam<std::vector<VariableName>>("displacements");

  // Create pressure BCs
  for (unsigned int i = 0; i < displacements.size(); ++i)
  {
    // Create unique kernel name for each of the components
    std::string unique_kernel_name = kernel_name + "_" + _name + "_" + Moose::stringify(i);

    InputParameters params = _factory.getValidParams(kernel_name);
    params.applyParameters(parameters(), {"factor"});
    params.set<bool>("use_displaced_mesh") = getParam<bool>("use_displaced_mesh");
    params.set<Real>("alpha") =
        isParamValid("alpha") ? getParam<Real>("alpha") : getParam<Real>("hht_alpha");

    params.set<NonlinearVariableName>("variable") = displacements[i];

    if (_has_save_in_vars[i])
      params.set<std::vector<AuxVariableName>>("save_in") = _save_in_vars[i];

    params.set<Real>("factor") = getParam<Real>("factor");
    _problem->addBoundaryCondition(kernel_name, unique_kernel_name, params);
  }
}
