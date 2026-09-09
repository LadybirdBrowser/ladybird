/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/GenericLexer.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/Vector.h>
#include <LibMedia/Export.h>

namespace Media::Codecs {

class H265 {
public:
    // ITU-T H.265 (07/2024), Annex A, limited to the profiles a representative record was captured for. The range
    // extensions profiles share one profile_idc and are told apart by the bit depth and chroma format they allow.
    enum class Profile : u8 {
        Main,
        Main10,
        Main12,
        Main422_10,
        Main422_12,
        Main444,
        Main444_10,
        Main444_12,
    };

    struct Parameters {
        Array<u8, 6> constraint_indicator_flags;
        u32 profile_compatibility_flags { 0 };
        u8 profile_space { 0 };
        u8 profile_idc { 0 };
        u8 level_idc { 0 };
        bool tier_flag { false };

        // The most constrained profile the stream states conformance to, which is the one a decoder need only
        // support to decode it. Empty for profiles no record was captured for.
        MEDIA_API Optional<Profile> profile() const;

        bool operator==(Parameters const&) const = default;
    };

    static Optional<Parameters> parse_codec_parameters(GenericLexer&);
    static MEDIA_API Optional<Parameters> parse_configuration_record(ReadonlyBytes);

    // The parameter sets a decoder is configured from. They are views into the configuration record they were read
    // from, so they live only as long as it does. Any of the collections may be empty.
    struct ParameterSets {
        Vector<ReadonlyBytes, 2> video;
        Vector<ReadonlyBytes, 2> sequence;
        Vector<ReadonlyBytes, 2> picture;
        u8 nal_unit_length_size { 4 };
    };

    struct NALUnitHeader {
        u8 nal_unit_type;
        u8 nuh_layer_id;
        u8 temporal_id;
    };

    struct VideoParameterSet {
        u8 vps_video_parameter_set_id;
    };

    struct SequenceParameterSet {
        u8 sps_seq_parameter_set_id;
        u8 sps_video_parameter_set_id;
        u8 sps_max_sub_layers_minus1;
        u8 sps_max_num_reorder_pics;
        u8 bit_depth_luma;
    };

    struct PictureParameterSet {
        u8 pps_pic_parameter_set_id;
        u8 pps_seq_parameter_set_id;
    };

    // Latest definitions by ID, independently of which parameter sets are active (H.265, 7.4.2.4.1).
    class MEDIA_API ParameterSetStore {
    public:
        struct StoredVPS {
            VideoParameterSet parameters;
            ByteBuffer nal_unit;
        };
        struct StoredSPS {
            SequenceParameterSet parameters;
            ByteBuffer nal_unit;
        };
        struct StoredPPS {
            PictureParameterSet parameters;
            ByteBuffer nal_unit;
        };

        ParameterSetStore();

        // Returns true for each set that defines something the store did not already hold. A repeated definition,
        // another NAL type, or an enhancement layer's set all return false. A failed update preserves the existing
        // definition.
        ErrorOr<bool> apply_nal_unit(ReadonlyBytes);

        // Views and pointers must be reacquired after updates, which can grow the backing vectors.
        Span<StoredVPS const> video_parameter_sets() const { return m_video; }
        Span<StoredSPS const> sequence_parameter_sets() const { return m_sequence; }
        Span<StoredPPS const> picture_parameter_sets() const { return m_picture; }
        StoredVPS const* video_parameter_set(u8 id) const;
        StoredSPS const* sequence_parameter_set(u8 id) const;
        StoredPPS const* picture_parameter_set(u8 id) const;

    private:
        static constexpr u16 ABSENT = 0xffff;
        // ITU-T H.265 (07/2024), 7.4.3.1, 7.4.3.2.1 and 7.4.3.3.1: 16 VPS IDs, 16 SPS IDs and 64 PPS IDs.
        Array<u16, 16> m_video_indices;
        Array<u16, 16> m_sequence_indices;
        Array<u16, 64> m_picture_indices;
        Vector<StoredVPS, 2> m_video;
        Vector<StoredSPS, 2> m_sequence;
        Vector<StoredPPS, 2> m_picture;
    };

    // Unlike H.264's single byte, an H.265 NAL unit header is two, so it cannot be read from the first byte alone.
    static MEDIA_API Optional<NALUnitHeader> parse_nal_unit_header(ReadonlyBytes nal_unit);
    static MEDIA_API bool is_coded_slice(NALUnitHeader const&);
    static MEDIA_API bool is_sub_layer_non_reference(NALUnitHeader const&);

    // Parse the fields needed for parameter-set tracking and output reordering from a complete NAL unit,
    // including its header. These do not validate the remaining syntax of the parameter set.
    static MEDIA_API Optional<VideoParameterSet> parse_video_parameter_set(ReadonlyBytes nal_unit);
    static MEDIA_API Optional<SequenceParameterSet> parse_sequence_parameter_set(ReadonlyBytes nal_unit);
    static MEDIA_API Optional<PictureParameterSet> parse_picture_parameter_set(ReadonlyBytes nal_unit);

    static MEDIA_API Optional<ParameterSets> parse_parameter_sets_from_configuration_record(ReadonlyBytes configuration_record);

    // A configuration record standing in for streams carrying this profile, so that a platform decoder can be asked
    // what it supports before any stream has arrived. Empty for profiles that no record was captured for. The
    // constraint flags are needed because the range extensions profiles all share one profile_idc.
    static MEDIA_API ReadonlyBytes representative_configuration_record_for_profile(Profile);

    // Parameters that resolve to this profile and nothing more, for keying answers that depend only on it.
    static MEDIA_API Parameters canonical_parameters_for_profile(Profile);
};

}
