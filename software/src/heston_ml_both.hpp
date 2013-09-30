//
// Copyright (C) 2013 University of Kaiserslautern
// Microelectronic Systems Design Research Group
//
// Christian Brugger (brugger@eit.uni-kl.de)
// 30. September 2013
//

#ifndef __HESTON_ML_BOTH_HPP__
#define __HESTON_ML_BOTH_HPP__

#include "heston_common.hpp"

#ifdef WITH_MPI
	#include "mpi.h"
#endif

#include <functional>

float heston_ml_control(const HestonParamsML &ml_params,
		std::function<Statistics(const HestonParamsML, const uint32_t, 
		const uint32_t, const bool)> ml_kernel);

#endif
