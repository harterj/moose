//* This file is part of the MOOSE framework
//* https://www.mooseframework.org
//*
//* All rights reserved, see COPYRIGHT for full restrictions
//* https://github.com/idaholab/moose/blob/master/COPYRIGHT
//*
//* Licensed under LGPL 2.1, please see LICENSE for details
//* https://www.gnu.org/licenses/lgpl-2.1.html

#pragma once

#include "ADVectorIntegratedBC.h"

class INSADVaporRecoilPressureMomentumFluxBC : public ADVectorIntegratedBC
{
public:
  static InputParameters validParams();

  INSADVaporRecoilPressureMomentumFluxBC(const InputParameters & parameters);

protected:
  virtual ADReal computeQpResidual() override;

  /// The recoil pressure
  const ADMaterialProperty<Real> & _rc_pressure;
};
