/*
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Vector4.h>
#include <LibMedia/Color/CodingIndependentCodePoints.h>

namespace Media {

class TransferCharacteristicsConversion {
public:
    static float to_linear_luminance(float value, TransferCharacteristics transfer_function);

    static float to_non_linear_luminance(float value, TransferCharacteristics transfer_function);

    // https://en.wikipedia.org/wiki/Hybrid_log-gamma
    // See "HLG reference OOTF"
    static FloatVector4 hlg_opto_optical_transfer_function(FloatVector4 const& vector, float gamma, float gain);
};

}
