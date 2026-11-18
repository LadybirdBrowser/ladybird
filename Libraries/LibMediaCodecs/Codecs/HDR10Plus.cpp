/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMediaCodecs/BitReader.h>
#include <LibMediaCodecs/Codecs/HDR10Plus.h>

namespace Media::Codecs {

static constexpr u8 UNITED_STATES_COUNTRY_CODE = 0xB5;
static constexpr u16 SAMSUNG_TERMINAL_PROVIDER_CODE = 0x003C;
static constexpr u16 HDR10_PLUS_TERMINAL_PROVIDER_ORIENTED_CODE = 0x0001;
static constexpr u8 HDR10_PLUS_APPLICATION_IDENTIFIER = 4;
static constexpr u8 LATEST_APPLICATION_VERSION = 1;

// The corners, ellipse and overlap option of each processing window after the first, which covers the whole picture.
static constexpr size_t PROCESSING_WINDOW_BIT_COUNT = 153;
static constexpr size_t TARGETED_SYSTEM_DISPLAY_MAXIMUM_LUMINANCE_BIT_COUNT = 27;
static constexpr size_t ACTUAL_PEAK_LUMINANCE_DIMENSION_BIT_COUNT = 5;
static constexpr size_t ACTUAL_PEAK_LUMINANCE_BIT_COUNT = 4;
static constexpr size_t MAXSCL_BIT_COUNT = 17;
static constexpr u32 MAXIMUM_MAXSCL = 100'000;

Optional<float> hdr10_plus_maximum_scene_luminance(ReadonlyBytes t35_message)
{
    BitReader reader { t35_message };
    if (reader.read_bits<u8>(8) != UNITED_STATES_COUNTRY_CODE)
        return {};
    if (reader.read_bits<u16>(16) != SAMSUNG_TERMINAL_PROVIDER_CODE)
        return {};
    if (reader.read_bits<u16>(16) != HDR10_PLUS_TERMINAL_PROVIDER_ORIENTED_CODE)
        return {};
    if (reader.read_bits<u8>(8) != HDR10_PLUS_APPLICATION_IDENTIFIER)
        return {};
    if (reader.read_bits<u8>(8) > LATEST_APPLICATION_VERSION)
        return {};

    auto window_count = reader.read_bits<u8>(2);
    if (window_count == 0)
        return {};
    reader.skip_bits(PROCESSING_WINDOW_BIT_COUNT * (window_count - 1));

    reader.skip_bits(TARGETED_SYSTEM_DISPLAY_MAXIMUM_LUMINANCE_BIT_COUNT);
    if (reader.read_bit()) {
        auto rows = reader.read_bits<u8>(ACTUAL_PEAK_LUMINANCE_DIMENSION_BIT_COUNT);
        auto columns = reader.read_bits<u8>(ACTUAL_PEAK_LUMINANCE_DIMENSION_BIT_COUNT);
        reader.skip_bits(static_cast<size_t>(rows) * columns * ACTUAL_PEAK_LUMINANCE_BIT_COUNT);
    }

    // The first window's maxscl, in units of 0.00001 of the 10000 cd/m² that the PQ curve peaks at.
    u32 maximum_component = 0;
    for (size_t component = 0; component < 3; component++)
        maximum_component = max(maximum_component, reader.read_bits<u32>(MAXSCL_BIT_COUNT));
    if (reader.has_overrun() || maximum_component == 0 || maximum_component > MAXIMUM_MAXSCL)
        return {};
    constexpr float maxscl_units_per_candela_per_square_metre = 10.0f;
    return static_cast<float>(maximum_component) / maxscl_units_per_candela_per_square_metre;
}

}
