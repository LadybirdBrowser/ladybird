/*
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <AK/Math.h>
#include <AK/StdLibExtras.h>

#include "TransferCharacteristics.h"

namespace Media {

// SDR maximum luminance in candelas per meter squared
constexpr float sdr_max_luminance = 120.0f;

// sRGB
constexpr float srgb_inverse_beta = 0.0031308f;
constexpr float srgb_inverse_linear_coef = 12.92f;
constexpr float srgb_gamma = 2.4f;
constexpr float srgb_alpha = 1.055f;

// BT.601/BT.709/BT.2020 constants
constexpr float bt_601_beta = 0.018053968510807f;
constexpr float bt_601_linear_coef = 4.5f;
constexpr float bt_601_alpha = 1.0f + 5.5f * bt_601_beta;
constexpr float bt_601_gamma = 0.45f;

// Perceptual quantizer (SMPTE ST 2084) constants
constexpr float pq_m1 = 2610.0f / 16384.0f;
constexpr float pq_m2 = 128.0f * 2523.0f / 4096.0f;
constexpr float pq_c1 = 3424.0f / 4096.0f;
constexpr float pq_c2 = 32.0f * 2413.0f / 4096.0f;
constexpr float pq_c3 = 32.0f * 2392.0f / 4096.0f;
constexpr float pq_max_luminance = 10000.0f;

// Hybrid log-gamma constants
constexpr float hlg_a = 0.17883277f;
constexpr float hlg_b = 0.28466892f;
constexpr float hlg_c = 0.55991073f;

float TransferCharacteristicsConversion::to_linear_luminance(float value, TransferCharacteristics transfer_function)
{
    switch (transfer_function) {
    case TransferCharacteristics::BT709:
    case TransferCharacteristics::BT601:
    case TransferCharacteristics::BT2020BitDepth10:
    case TransferCharacteristics::BT2020BitDepth12:
        // https://en.wikipedia.org/wiki/Rec._601#Transfer_characteristics
        // https://en.wikipedia.org/wiki/Rec._709#Transfer_characteristics
        // https://en.wikipedia.org/wiki/Rec._2020#Transfer_characteristics
        // These three share identical OETFs.
        if (value < bt_601_beta * bt_601_linear_coef)
            return value / bt_601_linear_coef;
        return AK::pow((value + (bt_601_alpha - 1.0f)) / bt_601_alpha, 1.0f / bt_601_gamma);
    case TransferCharacteristics::SRGB:
        // https://color.org/sRGB.pdf
        if (value < srgb_inverse_linear_coef * srgb_inverse_beta)
            return value / srgb_inverse_linear_coef;
        return AK::pow((value + (srgb_alpha - 1.0f)) / srgb_alpha, srgb_gamma);
    case TransferCharacteristics::SMPTE2084: {
        // https://en.wikipedia.org/wiki/Perceptual_quantizer
        auto gamma_adjusted = AK::pow(value, 1.0f / pq_m2);
        auto numerator = max(gamma_adjusted - pq_c1, 0.0f);
        auto denominator = pq_c2 - pq_c3 * gamma_adjusted;
        return AK::pow(numerator / denominator, 1.0f / pq_m1) * (pq_max_luminance / sdr_max_luminance);
    }
    case TransferCharacteristics::HLG:
        // https://en.wikipedia.org/wiki/Hybrid_log-gamma
        if (value < 0.5f)
            return (value * value) / 3.0f;
        return (AK::exp((value - hlg_c) / hlg_a) + hlg_b) / 12.0f;
    default:
        dbgln("Unsupported transfer function {}", static_cast<u8>(transfer_function));
        VERIFY_NOT_REACHED();
    }
}

float TransferCharacteristicsConversion::to_non_linear_luminance(float value, TransferCharacteristics transfer_function)
{
    switch (transfer_function) {
    case TransferCharacteristics::BT709:
    case TransferCharacteristics::BT601:
    case TransferCharacteristics::BT2020BitDepth10:
    case TransferCharacteristics::BT2020BitDepth12:
        // https://en.wikipedia.org/wiki/Rec._601#Transfer_characteristics
        // https://en.wikipedia.org/wiki/Rec._709#Transfer_characteristics
        // https://en.wikipedia.org/wiki/Rec._2020#Transfer_characteristics
        // These three share identical OETFs.
        if (value < bt_601_beta)
            return bt_601_linear_coef * value;
        return bt_601_alpha * AK::pow(value, bt_601_gamma) - (bt_601_alpha - 1.0f);
    case TransferCharacteristics::SRGB:
        // https://color.org/sRGB.pdf
        if (value < srgb_inverse_beta)
            return value * srgb_inverse_linear_coef;
        return srgb_alpha * AK::pow(value, 1.0f / srgb_gamma) - (srgb_alpha - 1.0f);
    case TransferCharacteristics::SMPTE2084: {
        // https://en.wikipedia.org/wiki/Perceptual_quantizer
        auto linear_value = AK::pow(value * (sdr_max_luminance / pq_max_luminance), pq_m1);
        auto numerator = pq_c1 + pq_c2 * linear_value;
        auto denominator = 1 + pq_c3 * linear_value;
        return AK::pow(numerator / denominator, pq_m2);
    }
    case TransferCharacteristics::HLG:
        // https://en.wikipedia.org/wiki/Hybrid_log-gamma
        if (value < 1.0f / 12.0f)
            return AK::sqrt(value * 3.0f);
        return hlg_a * AK::log(12.0f * value - hlg_b) + hlg_c;
    default:
        dbgln("Unsupported transfer function {}", static_cast<u8>(transfer_function));
        VERIFY_NOT_REACHED();
    }
}

FloatVector4 TransferCharacteristicsConversion::hlg_opto_optical_transfer_function(FloatVector4 const& vector, float gamma, float gain)
{
    float luminance = (0.2627f * vector.x() + 0.6780f * vector.y() + 0.0593f * vector.z()) * 1000.0f;
    float coefficient = gain * AK::pow(luminance, gamma - 1.0f);
    return FloatVector4(vector.x() * coefficient, vector.y() * coefficient, vector.z() * coefficient, vector.w());
}

}
