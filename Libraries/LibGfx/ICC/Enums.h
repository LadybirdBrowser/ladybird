/*
 * Copyright (c) 2023, Nico Weber <thakis@chromium.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>

namespace Gfx::ICC {

// ICC v4, 7.2.5 Profile/device class field
enum class DeviceClass : u32 {
    InputDevice = 0x73636E72,   // 'scnr'
    DisplayDevice = 0x6D6E7472, // 'mntr'
    OutputDevice = 0x70727472,  // 'prtr'
    DeviceLink = 0x6C696E6B,    // 'link'
    ColorSpace = 0x73706163,    // 'spac'
    Abstract = 0x61627374,      // 'abst'
    NamedColor = 0x6E6D636C,    // 'nmcl'
};
StringView device_class_name(DeviceClass);

// ICC v4, 7.2.6 Data colour space field, Table 19 — Data colour space signatures
enum class ColorSpace : u32 {
    nCIEXYZ = 0x58595A20,       // 'XYZ ', used in data color spaces.
    PCSXYZ = nCIEXYZ,           // Used in profile connection space instead.
    CIELAB = 0x4C616220,        // 'Lab ', used in data color spaces.
    PCSLAB = CIELAB,            // Used in profile connection space instead.
    CIELUV = 0x4C757620,        // 'Luv '
    YCbCr = 0x59436272,         // 'YCbr'
    CIEYxy = 0x59787920,        // 'Yxy '
    RGB = 0x52474220,           // 'RGB '
    Gray = 0x47524159,          // 'GRAY'
    HSV = 0x48535620,           // 'HSV '
    HLS = 0x484C5320,           // 'HLS '
    CMYK = 0x434D594B,          // 'CMYK'
    CMY = 0x434D5920,           // 'CMY '
    TwoColor = 0x32434C52,      // '2CLR'
    ThreeColor = 0x33434C52,    // '3CLR'
    FourColor = 0x34434C52,     // '4CLR'
    FiveColor = 0x35434C52,     // '5CLR'
    SixColor = 0x36434C52,      // '6CLR'
    SevenColor = 0x37434C52,    // '7CLR'
    EightColor = 0x38434C52,    // '8CLR'
    NineColor = 0x39434C52,     // '9CLR'
    TenColor = 0x41434C52,      // 'ACLR'
    ElevenColor = 0x42434C52,   // 'BCLR'
    TwelveColor = 0x43434C52,   // 'CCLR'
    ThirteenColor = 0x44434C52, // 'DCLR'
    FourteenColor = 0x45434C52, // 'ECLR'
    FifteenColor = 0x46434C52,  // 'FCLR'
};
StringView data_color_space_name(ColorSpace);
StringView profile_connection_space_name(ColorSpace);
unsigned number_of_components_in_color_space(ColorSpace);

// ICC v4, 7.2.10 Primary platform field, Table 20 — Primary platforms
enum class PrimaryPlatform : u32 {
    Apple = 0x4150504C,           // 'APPL'
    Microsoft = 0x4D534654,       // 'MSFT'
    SiliconGraphics = 0x53474920, // 'SGI '
    Sun = 0x53554E57,             // 'SUNW'
};
StringView primary_platform_name(PrimaryPlatform);

// ICC v4, 7.2.15 Rendering intent field
enum class RenderingIntent {
    Perceptual,
    MediaRelativeColorimetric,
    Saturation,
    ICCAbsoluteColorimetric,
};
StringView rendering_intent_name(RenderingIntent);

}
