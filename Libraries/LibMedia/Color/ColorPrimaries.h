/*
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Matrix3x3.h>
#include <LibMedia/Color/CodingIndependentCodePoints.h>
#include <LibMedia/DecoderError.h>

namespace Media {

DecoderErrorOr<FloatMatrix3x3> get_conversion_matrix(ColorPrimaries input_primaries, ColorPrimaries output_primaries);

}
