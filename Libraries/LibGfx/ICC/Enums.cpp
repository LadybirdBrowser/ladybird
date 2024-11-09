/*
 * Copyright (c) 2023, Nico Weber <thakis@chromium.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/ICC/Enums.h>

namespace Gfx::ICC {

StringView device_class_name(DeviceClass device_class)
{
    switch (device_class) {
    case DeviceClass::InputDevice:
        return "InputDevice"sv;
    case DeviceClass::DisplayDevice:
        return "DisplayDevice"sv;
    case DeviceClass::OutputDevice:
        return "OutputDevice"sv;
    case DeviceClass::DeviceLink:
        return "DeviceLink"sv;
    case DeviceClass::ColorSpace:
        return "ColorSpace"sv;
    case DeviceClass::Abstract:
        return "Abstract"sv;
    case DeviceClass::NamedColor:
        return "NamedColor"sv;
    }
    VERIFY_NOT_REACHED();
}

StringView data_color_space_name(ColorSpace color_space)
{
    switch (color_space) {
    case ColorSpace::nCIEXYZ:
        return "nCIEXYZ"sv;
    case ColorSpace::CIELAB:
        return "CIELAB"sv;
    case ColorSpace::CIELUV:
        return "CIELUV"sv;
    case ColorSpace::YCbCr:
        return "YCbCr"sv;
    case ColorSpace::CIEYxy:
        return "CIEYxy"sv;
    case ColorSpace::RGB:
        return "RGB"sv;
    case ColorSpace::Gray:
        return "Gray"sv;
    case ColorSpace::HSV:
        return "HSV"sv;
    case ColorSpace::HLS:
        return "HLS"sv;
    case ColorSpace::CMYK:
        return "CMYK"sv;
    case ColorSpace::CMY:
        return "CMY"sv;
    case ColorSpace::TwoColor:
        return "2 color"sv;
    case ColorSpace::ThreeColor:
        return "3 color (other than XYZ, Lab, Luv, YCbCr, CIEYxy, RGB, HSV, HLS, CMY)"sv;
    case ColorSpace::FourColor:
        return "4 color (other than CMYK)"sv;
    case ColorSpace::FiveColor:
        return "5 color"sv;
    case ColorSpace::SixColor:
        return "6 color"sv;
    case ColorSpace::SevenColor:
        return "7 color"sv;
    case ColorSpace::EightColor:
        return "8 color"sv;
    case ColorSpace::NineColor:
        return "9 color"sv;
    case ColorSpace::TenColor:
        return "10 color"sv;
    case ColorSpace::ElevenColor:
        return "11 color"sv;
    case ColorSpace::TwelveColor:
        return "12 color"sv;
    case ColorSpace::ThirteenColor:
        return "13 color"sv;
    case ColorSpace::FourteenColor:
        return "14 color"sv;
    case ColorSpace::FifteenColor:
        return "15 color"sv;
    }
    VERIFY_NOT_REACHED();
}

StringView profile_connection_space_name(ColorSpace color_space)
{
    switch (color_space) {
    case ColorSpace::PCSXYZ:
        return "PCSXYZ"sv;
    case ColorSpace::PCSLAB:
        return "PCSLAB"sv;
    default:
        return data_color_space_name(color_space);
    }
}

unsigned number_of_components_in_color_space(ColorSpace color_space)
{
    switch (color_space) {
    case ColorSpace::Gray:
        return 1;
    case ColorSpace::TwoColor:
        return 2;
    case ColorSpace::nCIEXYZ:
    case ColorSpace::CIELAB:
    case ColorSpace::CIELUV:
    case ColorSpace::YCbCr:
    case ColorSpace::CIEYxy:
    case ColorSpace::RGB:
    case ColorSpace::HSV:
    case ColorSpace::HLS:
    case ColorSpace::CMY:
    case ColorSpace::ThreeColor:
        return 3;
    case ColorSpace::CMYK:
    case ColorSpace::FourColor:
        return 4;
    case ColorSpace::FiveColor:
        return 5;
    case ColorSpace::SixColor:
        return 6;
    case ColorSpace::SevenColor:
        return 7;
    case ColorSpace::EightColor:
        return 8;
    case ColorSpace::NineColor:
        return 9;
    case ColorSpace::TenColor:
        return 10;
    case ColorSpace::ElevenColor:
        return 11;
    case ColorSpace::TwelveColor:
        return 12;
    case ColorSpace::ThirteenColor:
        return 13;
    case ColorSpace::FourteenColor:
        return 14;
    case ColorSpace::FifteenColor:
        return 15;
    }
    VERIFY_NOT_REACHED();
}

StringView primary_platform_name(PrimaryPlatform primary_platform)
{
    switch (primary_platform) {
    case PrimaryPlatform::Apple:
        return "Apple"sv;
    case PrimaryPlatform::Microsoft:
        return "Microsoft"sv;
    case PrimaryPlatform::SiliconGraphics:
        return "Silicon Graphics"sv;
    case PrimaryPlatform::Sun:
        return "Sun"sv;
    }
    VERIFY_NOT_REACHED();
}

StringView rendering_intent_name(RenderingIntent rendering_intent)
{
    switch (rendering_intent) {
    case RenderingIntent::Perceptual:
        return "Perceptual"sv;
    case RenderingIntent::MediaRelativeColorimetric:
        return "Media-relative colorimetric"sv;
    case RenderingIntent::Saturation:
        return "Saturation"sv;
    case RenderingIntent::ICCAbsoluteColorimetric:
        return "ICC-absolute colorimetric"sv;
    }
    VERIFY_NOT_REACHED();
}

}
