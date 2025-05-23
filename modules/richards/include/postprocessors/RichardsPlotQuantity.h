//* This file is part of the MOOSE framework
//* https://www.mooseframework.org
//*
//* All rights reserved, see COPYRIGHT for full restrictions
//* https://github.com/idaholab/moose/blob/master/COPYRIGHT
//*
//* Licensed under LGPL 2.1, please see LICENSE for details
//* https://www.gnu.org/licenses/lgpl-2.1.html

#pragma once

#include "GeneralPostprocessor.h"

class RichardsSumQuantity;

/**
 * Extracts the value from RichardsSumQuantity userobject
 */
class RichardsPlotQuantity : public GeneralPostprocessor
{
public:
  static InputParameters validParams();

  RichardsPlotQuantity(const InputParameters & parameters);
  virtual ~RichardsPlotQuantity();

  virtual void initialize() override;
  virtual void execute() override;

  /// returns the value of the RichardsSumQuantity
  using Postprocessor::getValue;
  virtual PostprocessorValue getValue() const override;

protected:
  /// the RichardsSumQuantity userobject
  const RichardsSumQuantity & _total_mass;
};
