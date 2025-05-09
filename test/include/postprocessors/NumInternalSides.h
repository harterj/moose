//* This file is part of the MOOSE framework
//* https://www.mooseframework.org
//*
//* All rights reserved, see COPYRIGHT for full restrictions
//* https://github.com/idaholab/moose/blob/master/COPYRIGHT
//*
//* Licensed under LGPL 2.1, please see LICENSE for details
//* https://www.gnu.org/licenses/lgpl-2.1.html

#pragma once

// MOOSE includes
#include "InternalSidePostprocessor.h"

/**
 * An object for testing the block restricted behavior of InternalSideUserObject, it
 * simply counts the number of sides
 */
class NumInternalSides : public InternalSidePostprocessor
{
public:
  static InputParameters validParams();

  NumInternalSides(const InputParameters & parameters);
  virtual ~NumInternalSides();
  virtual void execute() override;
  virtual void threadJoin(const UserObject & uo) override;
  virtual void finalize() override;
  virtual void initialize() override;
  using Postprocessor::getValue;
  virtual PostprocessorValue getValue() const override;
  const unsigned int & count() const { return _count; }

private:
  unsigned int _count;
};
