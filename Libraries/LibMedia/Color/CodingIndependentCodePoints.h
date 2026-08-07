/*
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Format.h>
#include <AK/StringView.h>

namespace Media {

// CICP is defined by H.273:
// https://www.itu.int/rec/T-REC-H.273/en
// See the Section 8.
// Current edition is from 07/21.

enum class ColorPrimaries : u8 {
    // Reserved
    BT709 = 1,
    Unspecified = 2, // Used by codecs to indicate that an alternative value may be used.
    // Reserved
    BT470M = 4,
    BT470BG = 5,
    BT601 = 6,
    SMPTE240 = 7,
    GenericFilm = 8,
    BT2020 = 9,
    XYZ = 10,
    SMPTE431 = 11,
    SMPTE432 = 12,
    EBU3213 = 22,
};

enum class TransferCharacteristics : u8 {
    // Reserved
    BT709 = 1,
    Unspecified = 2, // Used by codecs to indicate that an alternative value may be used
    // Reserved
    BT470M = 4,
    BT470BG = 5,
    BT601 = 6, // BT.601 or Rec. 601
    SMPTE240 = 7,
    Linear = 8,
    Log100 = 9,
    Log100Sqrt10 = 10,
    IEC61966 = 11,
    BT1361 = 12,
    SRGB = 13,
    BT2020BitDepth10 = 14,
    BT2020BitDepth12 = 15,
    SMPTE2084 = 16, // Also known as PQ
    SMPTE428 = 17,
    HLG = 18,
};

enum class MatrixCoefficients : u8 {
    Identity = 0,
    BT709 = 1,
    Unspecified = 2, // Used by codecs to indicate that an alternative value may be used.
    // Reserved
    FCC = 4,
    BT470BG = 5,
    BT601 = 6,
    SMPTE240 = 7,
    YCgCo = 8,
    BT2020NonConstantLuminance = 9,
    BT2020ConstantLuminance = 10,
    SMPTE2085 = 11,
    ChromaticityDerivedNonConstantLuminance = 12,
    ChromaticityDerivedConstantLuminance = 13,
    ICtCp = 14,
};

enum class VideoFullRangeFlag : u8 {
    Studio = 0,      // Y range 16..235, UV range 16..240
    Full = 1,        // 0..255
    Unspecified = 2, // Used by codecs to indicate that an alternative value may be used.
};

static constexpr bool color_primaries_valid(ColorPrimaries color_primaries)
{
    switch (color_primaries) {
    case ColorPrimaries::Unspecified:
        return false;
    case ColorPrimaries::BT709:
    case ColorPrimaries::BT470M:
    case ColorPrimaries::BT470BG:
    case ColorPrimaries::BT601:
    case ColorPrimaries::SMPTE240:
    case ColorPrimaries::GenericFilm:
    case ColorPrimaries::BT2020:
    case ColorPrimaries::XYZ:
    case ColorPrimaries::SMPTE431:
    case ColorPrimaries::SMPTE432:
    case ColorPrimaries::EBU3213:
        return true;
    }
    return false;
}

static constexpr bool transfer_characteristics_valid(TransferCharacteristics transfer_characteristics)
{
    switch (transfer_characteristics) {
    case TransferCharacteristics::Unspecified:
        return false;
    case TransferCharacteristics::BT709:
    case TransferCharacteristics::BT470M:
    case TransferCharacteristics::BT470BG:
    case TransferCharacteristics::BT601:
    case TransferCharacteristics::SMPTE240:
    case TransferCharacteristics::Linear:
    case TransferCharacteristics::Log100:
    case TransferCharacteristics::Log100Sqrt10:
    case TransferCharacteristics::IEC61966:
    case TransferCharacteristics::BT1361:
    case TransferCharacteristics::SRGB:
    case TransferCharacteristics::BT2020BitDepth10:
    case TransferCharacteristics::BT2020BitDepth12:
    case TransferCharacteristics::SMPTE2084:
    case TransferCharacteristics::SMPTE428:
    case TransferCharacteristics::HLG:
        return true;
    }
    return false;
}

static constexpr bool matrix_coefficients_valid(MatrixCoefficients matrix_coefficients)
{
    switch (matrix_coefficients) {
    case MatrixCoefficients::Unspecified:
        return false;
    case MatrixCoefficients::Identity:
    case MatrixCoefficients::BT709:
    case MatrixCoefficients::FCC:
    case MatrixCoefficients::BT470BG:
    case MatrixCoefficients::BT601:
    case MatrixCoefficients::SMPTE240:
    case MatrixCoefficients::YCgCo:
    case MatrixCoefficients::BT2020NonConstantLuminance:
    case MatrixCoefficients::BT2020ConstantLuminance:
    case MatrixCoefficients::SMPTE2085:
    case MatrixCoefficients::ChromaticityDerivedNonConstantLuminance:
    case MatrixCoefficients::ChromaticityDerivedConstantLuminance:
    case MatrixCoefficients::ICtCp:
        return true;
    }
    return false;
}

static constexpr bool video_full_range_flag_valid(VideoFullRangeFlag video_full_range_flag)
{
    switch (video_full_range_flag) {
    case VideoFullRangeFlag::Unspecified:
        return false;
    case VideoFullRangeFlag::Studio:
    case VideoFullRangeFlag::Full:
        return true;
    }
    return false;
}

// https://en.wikipedia.org/wiki/Coding-independent_code_points
struct CodingIndependentCodePoints {
public:
    constexpr CodingIndependentCodePoints() = default;

    constexpr CodingIndependentCodePoints(ColorPrimaries color_primaries, TransferCharacteristics transfer_characteristics, MatrixCoefficients matrix_coefficients, VideoFullRangeFlag video_full_range_flag)
        : m_color_primaries(color_primaries)
        , m_transfer_characteristics(transfer_characteristics)
        , m_matrix_coefficients(matrix_coefficients)
        , m_video_full_range_flag(video_full_range_flag)
    {
    }

    constexpr ColorPrimaries color_primaries() const { return m_color_primaries; }
    constexpr void set_color_primaries(ColorPrimaries value) { m_color_primaries = value; }
    constexpr TransferCharacteristics transfer_characteristics() const { return m_transfer_characteristics; }
    constexpr void set_transfer_characteristics(TransferCharacteristics value) { m_transfer_characteristics = value; }
    constexpr MatrixCoefficients matrix_coefficients() const { return m_matrix_coefficients; }
    constexpr void set_matrix_coefficients(MatrixCoefficients value) { m_matrix_coefficients = value; }
    constexpr VideoFullRangeFlag video_full_range_flag() const { return m_video_full_range_flag; }
    constexpr void set_video_full_range_flag(VideoFullRangeFlag value) { m_video_full_range_flag = value; }

    constexpr bool operator==(CodingIndependentCodePoints const&) const = default;

    constexpr bool is_valid_or_unspecified() const
    {
        return (color_primaries_valid(m_color_primaries) || m_color_primaries == ColorPrimaries::Unspecified)
            && (transfer_characteristics_valid(m_transfer_characteristics) || m_transfer_characteristics == TransferCharacteristics::Unspecified)
            && (matrix_coefficients_valid(m_matrix_coefficients) || m_matrix_coefficients == MatrixCoefficients::Unspecified)
            && (video_full_range_flag_valid(m_video_full_range_flag) || m_video_full_range_flag == VideoFullRangeFlag::Unspecified);
    }

    constexpr void adopt_specified_values(CodingIndependentCodePoints cicp)
    {
        if (color_primaries_valid(cicp.color_primaries()))
            set_color_primaries(cicp.color_primaries());
        if (transfer_characteristics_valid(cicp.transfer_characteristics()))
            set_transfer_characteristics(cicp.transfer_characteristics());
        if (matrix_coefficients_valid(cicp.matrix_coefficients()))
            set_matrix_coefficients(cicp.matrix_coefficients());
        if (video_full_range_flag_valid(cicp.video_full_range_flag()))
            set_video_full_range_flag(cicp.video_full_range_flag());
    }

private:
    ColorPrimaries m_color_primaries = ColorPrimaries::Unspecified;
    TransferCharacteristics m_transfer_characteristics = TransferCharacteristics::Unspecified;
    MatrixCoefficients m_matrix_coefficients = MatrixCoefficients::Unspecified;
    VideoFullRangeFlag m_video_full_range_flag = VideoFullRangeFlag::Unspecified;
};

constexpr StringView color_primaries_to_string(ColorPrimaries color_primaries)
{
    switch (color_primaries) {
    case ColorPrimaries::BT709:
        return "BT.709"sv;
    case ColorPrimaries::Unspecified:
        return "Unspecified"sv;
    case ColorPrimaries::BT470M:
        return "BT.470 System M"sv;
    case ColorPrimaries::BT470BG:
        return "BT.470 System B, G"sv;
    case ColorPrimaries::BT601:
        return "BT.601"sv;
    case ColorPrimaries::SMPTE240:
        return "SMPTE ST 240"sv;
    case ColorPrimaries::GenericFilm:
        return "Generic film"sv;
    case ColorPrimaries::BT2020:
        return "BT.2020"sv;
    case ColorPrimaries::XYZ:
        return "CIE 1931 XYZ"sv;
    case ColorPrimaries::SMPTE431:
        return "SMPTE RP 431"sv;
    case ColorPrimaries::SMPTE432:
        return "SMPTE EG 432"sv;
    case ColorPrimaries::EBU3213:
        return "EBU Tech 3213"sv;
    }
    return "Reserved"sv;
}

constexpr StringView transfer_characteristics_to_string(TransferCharacteristics transfer_characteristics)
{
    switch (transfer_characteristics) {
    case TransferCharacteristics::BT709:
        return "BT.709"sv;
    case TransferCharacteristics::Unspecified:
        return "Unspecified"sv;
    case TransferCharacteristics::BT470M:
        return "BT.470 System M"sv;
    case TransferCharacteristics::BT470BG:
        return "BT.470 System B, G"sv;
    case TransferCharacteristics::BT601:
        return "BT.601"sv;
    case TransferCharacteristics::SMPTE240:
        return "SMPTE ST 240"sv;
    case TransferCharacteristics::Linear:
        return "Linear"sv;
    case TransferCharacteristics::Log100:
        return "Logarithmic (100:1 range)"sv;
    case TransferCharacteristics::Log100Sqrt10:
        return "Logarithmic (100xSqrt(10):1 range)"sv;
    case TransferCharacteristics::IEC61966:
        return "IEC 61966"sv;
    case TransferCharacteristics::BT1361:
        return "BT.1361"sv;
    case TransferCharacteristics::SRGB:
        return "sRGB"sv;
    case TransferCharacteristics::BT2020BitDepth10:
        return "BT.2020 (10-bit)"sv;
    case TransferCharacteristics::BT2020BitDepth12:
        return "BT.2020 (12-bit)"sv;
    case TransferCharacteristics::SMPTE2084:
        return "SMPTE ST 2084 (PQ)"sv;
    case TransferCharacteristics::SMPTE428:
        return "SMPTE ST 428"sv;
    case TransferCharacteristics::HLG:
        return "ARIB STD-B67 (HLG, BT.2100)"sv;
    }
    return "Reserved"sv;
}

constexpr StringView matrix_coefficients_to_string(MatrixCoefficients matrix_coefficients)
{
    switch (matrix_coefficients) {
    case MatrixCoefficients::Identity:
        return "Identity"sv;
    case MatrixCoefficients::BT709:
        return "BT.709"sv;
    case MatrixCoefficients::Unspecified:
        return "Unspecified"sv;
    case MatrixCoefficients::FCC:
        return "FCC (CFR 73.682)"sv;
    case MatrixCoefficients::BT470BG:
        return "BT.470 System B, G"sv;
    case MatrixCoefficients::BT601:
        return "BT.601"sv;
    case MatrixCoefficients::SMPTE240:
        return "SMPTE ST 240"sv;
    case MatrixCoefficients::YCgCo:
        return "YCgCo"sv;
    case MatrixCoefficients::BT2020NonConstantLuminance:
        return "BT.2020, non-constant luminance"sv;
    case MatrixCoefficients::BT2020ConstantLuminance:
        return "BT.2020, constant luminance"sv;
    case MatrixCoefficients::SMPTE2085:
        return "SMPTE ST 2085"sv;
    case MatrixCoefficients::ChromaticityDerivedNonConstantLuminance:
        return "Chromaticity-derived, non-constant luminance"sv;
    case MatrixCoefficients::ChromaticityDerivedConstantLuminance:
        return "Chromaticity-derived, constant luminance"sv;
    case MatrixCoefficients::ICtCp:
        return "BT.2100 ICtCp"sv;
    }
    return "Reserved"sv;
}

constexpr StringView video_full_range_flag_to_string(VideoFullRangeFlag video_full_range_flag)
{
    switch (video_full_range_flag) {
    case VideoFullRangeFlag::Studio:
        return "Studio"sv;
    case VideoFullRangeFlag::Full:
        return "Full"sv;
    case VideoFullRangeFlag::Unspecified:
        return "Unspecified"sv;
    }
    return "Unknown"sv;
}

}

namespace AK {

template<>
struct Formatter<Media::ColorPrimaries> final : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Media::ColorPrimaries color_primaries)
    {
        return Formatter<StringView>::format(builder, Media::color_primaries_to_string(color_primaries));
    }
};

template<>
struct Formatter<Media::TransferCharacteristics> final : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Media::TransferCharacteristics transfer_characteristics)
    {
        return Formatter<StringView>::format(builder, Media::transfer_characteristics_to_string(transfer_characteristics));
    }
};

template<>
struct Formatter<Media::MatrixCoefficients> final : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Media::MatrixCoefficients matrix_coefficients)
    {
        return Formatter<StringView>::format(builder, Media::matrix_coefficients_to_string(matrix_coefficients));
    }
};

template<>
struct Formatter<Media::VideoFullRangeFlag> final : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Media::VideoFullRangeFlag range)
    {
        return Formatter<StringView>::format(builder, Media::video_full_range_flag_to_string(range));
    }
};

template<>
struct Formatter<Media::CodingIndependentCodePoints> final : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, Media::CodingIndependentCodePoints cicp)
    {
        return Formatter<FormatString>::format(builder, "CICP {{ CP = {}, TC = {}, MC = {}, Range = {} }}"sv, cicp.color_primaries(), cicp.transfer_characteristics(), cicp.matrix_coefficients(), cicp.video_full_range_flag());
    }
};

}
