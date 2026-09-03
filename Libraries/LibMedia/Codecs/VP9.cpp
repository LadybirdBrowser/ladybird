/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericShorthands.h>
#include <LibMedia/BitReader.h>
#include <LibMedia/Codecs/CodecString.h>
#include <LibMedia/Codecs/VP9.h>

namespace Media::Codecs {

static bool is_valid_level(u8 level)
{
    return first_is_one_of(level, 0, 10, 11, 20, 21, 30, 31, 40, 41, 50, 51, 52, 60, 61, 62);
}

static bool profile_and_color_config_is_valid(u8 profile, u8 bit_depth, Subsampling subsampling)
{
    auto is_422_or_444 = first_is_one_of(subsampling, Subsampling::yuv422(), Subsampling::yuv444());

    switch (profile) {
    case 0:
        return bit_depth == 8 && subsampling == Subsampling::yuv420();
    case 1:
        return bit_depth == 8 && is_422_or_444;
    case 2:
        return first_is_one_of(bit_depth, 10, 12) && subsampling == Subsampling::yuv420();
    case 3:
        return first_is_one_of(bit_depth, 10, 12) && is_422_or_444;
    default:
        return false;
    }
}

static Optional<Subsampling> subsampling_from_encoded_value(u8 value)
{
    switch (value) {
    case 0:
    case 1:
        return Subsampling::yuv420();
    case 2:
        return Subsampling::yuv422();
    case 3:
        return Subsampling::yuv444();
    default:
        return {};
    }
}

// https://www.webmproject.org/vp9/mp4/
Optional<VP9::Parameters> VP9::parse_codec_parameters(GenericLexer& lexer)
{
    if (!lexer.consume_specific('.'))
        return {};
    auto profile = consume_two_digit_decimal(lexer);
    if (!lexer.consume_specific('.'))
        return {};
    auto level = consume_two_digit_decimal(lexer);
    if (!lexer.consume_specific('.'))
        return {};
    auto bit_depth = consume_two_digit_decimal(lexer);
    if (!profile.has_value() || !level.has_value() || !bit_depth.has_value())
        return {};
    if (!is_valid_level(*level) || !first_is_one_of(*bit_depth, 8, 10, 12))
        return {};

    ColorParameters color_parameters;
    if (!lexer.is_eof()) {
        if (!lexer.consume_specific('.'))
            return {};
        auto parsed_chroma_subsampling = consume_two_digit_decimal(lexer);
        if (!lexer.consume_specific('.'))
            return {};
        auto parsed_color_primaries = consume_two_digit_decimal(lexer);
        if (!lexer.consume_specific('.'))
            return {};
        auto parsed_transfer_characteristics = consume_two_digit_decimal(lexer);
        if (!lexer.consume_specific('.'))
            return {};
        auto parsed_matrix_coefficients = consume_two_digit_decimal(lexer);
        if (!lexer.consume_specific('.'))
            return {};
        auto video_full_range = consume_two_digit_decimal(lexer);
        if (!parsed_chroma_subsampling.has_value() || !parsed_color_primaries.has_value() || !parsed_transfer_characteristics.has_value() || !parsed_matrix_coefficients.has_value() || !video_full_range.has_value() || !lexer.is_eof())
            return {};

        auto subsampling = subsampling_from_encoded_value(*parsed_chroma_subsampling);
        if (!subsampling.has_value())
            return {};
        if (*video_full_range > 1)
            return {};

        auto parsed_cicp = CodingIndependentCodePoints {
            static_cast<ColorPrimaries>(*parsed_color_primaries),
            static_cast<TransferCharacteristics>(*parsed_transfer_characteristics),
            static_cast<MatrixCoefficients>(*parsed_matrix_coefficients),
            *video_full_range == 1 ? VideoFullRangeFlag::Full : VideoFullRangeFlag::Studio,
        };
        if (!parsed_cicp.is_valid_or_unspecified())
            return {};

        color_parameters.subsampling = *subsampling;
        color_parameters.cicp = parsed_cicp;

        // If matrixCoefficients is 0 (RGB), then chroma subsampling MUST be 3 (4:4:4).
        if (color_parameters.cicp.matrix_coefficients() == MatrixCoefficients::Identity && *subsampling != Subsampling::yuv444())
            return {};
    }

    if (!profile_and_color_config_is_valid(*profile, *bit_depth, color_parameters.subsampling))
        return {};

    return Parameters {
        *profile,
        *level,
        *bit_depth,
        color_parameters,
    };
}

// https://www.webmproject.org/vp9/mp4/
Optional<VP9::Parameters> VP9::parse_configuration_record(ReadonlyBytes record)
{
    BitReader reader { record };
    auto profile = reader.read_bits<u8>(8);
    auto level = reader.read_bits<u8>(8);
    auto bit_depth = reader.read_bits<u8>(4);
    auto subsampling = subsampling_from_encoded_value(reader.read_bits<u8>(3));
    auto video_full_range = reader.read_bit() ? VideoFullRangeFlag::Full : VideoFullRangeFlag::Studio;
    auto color_primaries = reader.read_bits<u8>(8);
    auto transfer_characteristics = reader.read_bits<u8>(8);
    auto matrix_coefficients = reader.read_bits<u8>(8);
    auto codec_initialization_data_size = reader.read_bits<u16>(16);
    if (reader.has_overrun())
        return {};

    // codecInitializationDataSize MUST be 0 for VP8 and VP9.
    if (codec_initialization_data_size != 0)
        return {};

    if (!is_valid_level(level))
        return {};

    if (!first_is_one_of(bit_depth, 8, 10, 12))
        return {};

    if (!subsampling.has_value())
        return {};

    if (!profile_and_color_config_is_valid(profile, bit_depth, *subsampling))
        return {};

    auto cicp = CodingIndependentCodePoints {
        static_cast<ColorPrimaries>(color_primaries),
        static_cast<TransferCharacteristics>(transfer_characteristics),
        static_cast<MatrixCoefficients>(matrix_coefficients),
        video_full_range,
    };
    if (!cicp.is_valid_or_unspecified())
        return {};
    if (cicp.matrix_coefficients() == MatrixCoefficients::Identity && *subsampling != Subsampling::yuv444())
        return {};

    // Unlike a codec string, a record describes the file we were given, so an unknown level is not rejected.
    return Parameters {
        profile,
        level,
        bit_depth,
        ColorParameters { *subsampling, cicp },
    };
}

namespace {

// A coded frame may pack several VP9 frames, sized by a trailing index whose marker is repeated at both of its ends.
Vector<ReadonlyBytes> split_superframe(ReadonlyBytes coded_frame)
{
    static constexpr u8 SUPERFRAME_MARKER_MASK = 0xe0;
    static constexpr u8 SUPERFRAME_MARKER = 0xc0;

    Vector<ReadonlyBytes> frames;
    if (coded_frame.is_empty()) {
        return frames;
    }

    auto marker = coded_frame[coded_frame.size() - 1];
    auto has_superframe_index = (marker & SUPERFRAME_MARKER_MASK) == SUPERFRAME_MARKER;
    if (!has_superframe_index) {
        frames.append(coded_frame);
        return frames;
    }

    size_t frame_count = (marker & 0x7) + 1;
    size_t bytes_per_frame_size = ((marker >> 3) & 0x3) + 1;
    auto index_size = 2 + (frame_count * bytes_per_frame_size);
    auto index_is_complete = coded_frame.size() >= index_size && coded_frame[coded_frame.size() - index_size] == marker;
    if (!index_is_complete) {
        frames.append(coded_frame);
        return frames;
    }

    auto sizes = coded_frame.slice(coded_frame.size() - index_size + 1);
    auto frames_size = coded_frame.size() - index_size;
    size_t offset = 0;
    for (size_t frame = 0; frame < frame_count; frame++) {
        size_t frame_size = 0;
        for (size_t byte = 0; byte < bytes_per_frame_size; byte++)
            frame_size |= static_cast<size_t>(sizes[(frame * bytes_per_frame_size) + byte]) << (byte * 8);
        if (offset + frame_size > frames_size)
            break;
        frames.append(coded_frame.slice(offset, frame_size));
        offset += frame_size;
    }
    return frames;
}

Optional<VP9::ColorParameters> parse_color_config(BitReader& reader, u8 profile, u8& bit_depth)
{
    bit_depth = 8;
    auto profile_has_high_bit_depth = profile >= 2;
    if (profile_has_high_bit_depth) {
        auto is_twelve_bit = reader.read_bit();
        bit_depth = is_twelve_bit ? 12 : 10;
    }

    // VP9's color space cannot express primaries or transfer characteristics, so only the matrix is taken from the
    // bitstream and the container supplies the rest.
    auto matrix_coefficients = [&]() -> MatrixCoefficients {
        switch (reader.read_bits<u8>(3)) {
        case 1:
            return MatrixCoefficients::BT470BG;
        case 2:
            return MatrixCoefficients::BT709;
        case 3:
            return MatrixCoefficients::BT601;
        case 4:
            return MatrixCoefficients::SMPTE240;
        case 5:
            return MatrixCoefficients::BT2020NonConstantLuminance;
        case 7:
            return MatrixCoefficients::Identity;
        default:
            return MatrixCoefficients::Unspecified;
        }
    }();
    auto cicp = CodingIndependentCodePoints { ColorPrimaries::Unspecified, TransferCharacteristics::Unspecified, matrix_coefficients, VideoFullRangeFlag::Unspecified };

    auto is_rgb = matrix_coefficients == MatrixCoefficients::Identity;
    auto profile_codes_subsampling = profile == 1 || profile == 3;

    VP9::ColorParameters color_parameters;
    if (is_rgb) {
        cicp.set_video_full_range_flag(VideoFullRangeFlag::Full);
        color_parameters.subsampling = Subsampling(false, false);
        if (profile_codes_subsampling)
            reader.skip_bits(1); // reserved_zero
    } else {
        auto is_full_range = reader.read_bit();
        cicp.set_video_full_range_flag(is_full_range ? VideoFullRangeFlag::Full : VideoFullRangeFlag::Studio);
        if (profile_codes_subsampling) {
            auto subsampling_x = reader.read_bit();
            auto subsampling_y = reader.read_bit();
            reader.skip_bits(1); // reserved_zero
            color_parameters.subsampling = Subsampling(subsampling_x, subsampling_y);
        } else {
            color_parameters.subsampling = Subsampling::yuv420();
        }
    }

    color_parameters.cicp = cicp;
    if (reader.has_overrun())
        return {};
    return color_parameters;
}

Optional<VP9::FrameHeader> parse_single_frame_header(ReadonlyBytes frame)
{
    static constexpr u8 FRAME_MARKER = 2;
    static constexpr Array<u8, 3> FRAME_SYNC_CODE { 0x49, 0x83, 0x42 };

    BitReader reader { frame };
    auto frame_marker = reader.read_bits<u8>(2);
    if (frame_marker != FRAME_MARKER)
        return {};

    auto profile_low_bit = reader.read_bits<u8>(1);
    auto profile_high_bit = reader.read_bits<u8>(1);
    u8 profile = (profile_high_bit << 1) | profile_low_bit;
    if (profile == 3) {
        auto reserved_zero = reader.read_bit();
        if (reserved_zero)
            return {};
    }

    auto shows_existing_frame = reader.read_bit();
    if (shows_existing_frame)
        return {};

    auto is_keyframe = !reader.read_bit();
    auto show_frame = reader.read_bit();
    auto error_resilient_mode = reader.read_bit();

    auto read_sync_code = [&] {
        for (auto expected : FRAME_SYNC_CODE) {
            if (reader.read_bits<u8>(8) != expected)
                return false;
        }
        return true;
    };

    // We omit anything from here that is irrelevant to intra-only frames. We're only interested in changes that occur
    // in intra-only frames, since hardware decoders can't handle inter-frame resolution changes.
    VP9::FrameHeader header;
    if (is_keyframe) {
        if (!read_sync_code())
            return {};
        auto color_parameters = parse_color_config(reader, profile, header.bit_depth);
        if (!color_parameters.has_value())
            return {};
        header.color_parameters = *color_parameters;
    } else {
        if (show_frame)
            return {};
        auto is_intra_only = reader.read_bit();
        if (!is_intra_only)
            return {};
        if (!error_resilient_mode)
            reader.read_bits(2); // reset_frame_context
        if (!read_sync_code())
            return {};
        auto intra_only_codes_color_config = profile > 0;
        if (intra_only_codes_color_config) {
            auto color_parameters = parse_color_config(reader, profile, header.bit_depth);
            if (!color_parameters.has_value())
                return {};
            header.color_parameters = *color_parameters;
        } else {
            header.color_parameters = { Subsampling::yuv420(), { ColorPrimaries::Unspecified, TransferCharacteristics::Unspecified, MatrixCoefficients::BT470BG, VideoFullRangeFlag::Studio } };
        }
        reader.skip_bits(8); // refresh_frame_flags
    }

    header.profile = profile;
    header.size = { static_cast<int>(reader.read_bits<u32>(16)) + 1, static_cast<int>(reader.read_bits<u32>(16)) + 1 };
    if (reader.has_overrun())
        return {};
    return header;
}

}

Optional<VP9::FrameHeader> VP9::parse_frame_header(ReadonlyBytes coded_frame)
{
    Optional<FrameHeader> header;
    for (auto frame : split_superframe(coded_frame)) {
        if (auto frame_header = parse_single_frame_header(frame); frame_header.has_value())
            header = frame_header;
    }
    return header;
}

}
