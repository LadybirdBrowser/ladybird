/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/ByteBuffer.h>
#include <AK/GenericLexer.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/Vector.h>
#include <LibMedia/Export.h>

namespace Media::Codecs {

class H264 {
public:
    // ITU-T H.264 (08/2024), Table A-1, limited to the profiles a representative record was captured for. Ordered
    // by how much each one allows, so that the more constrained of two is the smaller.
    enum class Profile : u8 {
        Baseline,
        Main,
        High,
        High10,
        High422,
        High444,
    };

    struct Parameters {
        u8 profile_idc { 0 };
        u8 constraint_set_flags { 0 };
        u8 level_idc { 0 };

        // The most constrained profile the stream states conformance to, which is the one a decoder need only
        // support to decode it. Empty for profiles no record was captured for.
        MEDIA_API Optional<Profile> profile() const;

        bool operator==(Parameters const&) const = default;
    };

    static Optional<Parameters> parse_codec_parameters(GenericLexer&);
    static MEDIA_API Optional<Parameters> parse_configuration_record(ReadonlyBytes);

    // The parameter sets a decoder is configured from. They are views into the configuration record they were read
    // from, so they live only as long as it does. Either collection may be empty.
    struct ParameterSets {
        Vector<ReadonlyBytes, 2> sequence;
        Vector<ReadonlyBytes, 2> picture;
        u8 nal_unit_length_size { 4 };
    };

    struct SequenceParameterSet {
        u8 seq_parameter_set_id;
        u8 max_num_reorder_frames;
    };

    struct PictureParameterSet {
        u8 pic_parameter_set_id;
        u8 seq_parameter_set_id;
    };

    // Latest definitions by ID, independently of which parameter sets are active (H.264, 7.4.1.2.1).
    class MEDIA_API ParameterSetStore {
    public:
        struct StoredSPS {
            SequenceParameterSet parameters;
            ByteBuffer nal_unit;
        };
        struct StoredPPS {
            PictureParameterSet parameters;
            ByteBuffer nal_unit;
        };

        ParameterSetStore();

        // Returns true for each SPS/PPS that defines something the store did not already hold. A repeated
        // definition, another NAL type, all return false. A failed update preserves the existing definition.
        ErrorOr<bool> apply_nal_unit(ReadonlyBytes);

        // Views and pointers must be reacquired after updates, which can grow the backing vectors.
        Span<StoredSPS const> sequence_parameter_sets() const { return m_sequence; }
        Span<StoredPPS const> picture_parameter_sets() const { return m_picture; }
        StoredSPS const* sequence_parameter_set(u8 id) const;
        StoredPPS const* picture_parameter_set(u8 id) const;

    private:
        static constexpr u16 ABSENT = 0xffff;
        // ITU-T H.264 (08/2024), 7.4.2.1.1 and 7.4.2.2: 32 SPS IDs and 256 PPS IDs.
        // u16 leaves room for an absent sentinel even when every PPS ID has a vector entry.
        Array<u16, 32> m_sequence_indices;
        Array<u16, 256> m_picture_indices;
        Vector<StoredSPS, 2> m_sequence;
        Vector<StoredPPS, 2> m_picture;
    };

    // Read the NAL unit header byte's fields (H.264, 7.4.1 and Table 7-1).
    static MEDIA_API bool is_coded_slice(u8 nal_unit_header);
    static MEDIA_API u8 nal_ref_idc(u8 nal_unit_header);

    // Parse the fields needed for parameter-set tracking and output reordering from a complete NAL unit,
    // including its header. These do not validate the remaining syntax of the parameter set.
    static MEDIA_API Optional<SequenceParameterSet> parse_sequence_parameter_set(ReadonlyBytes nal_unit);
    static MEDIA_API Optional<PictureParameterSet> parse_picture_parameter_set(ReadonlyBytes nal_unit);

    static MEDIA_API Optional<ParameterSets> parse_parameter_sets_from_configuration_record(ReadonlyBytes configuration_record);

    // A configuration record standing in for streams carrying this profile, so that a platform decoder can be
    // asked what it supports before any stream has arrived. Empty for profiles that no record was captured for.
    // The constraint flags are needed because a stream may state that it also obeys a profile we do hold one for.
    static MEDIA_API ReadonlyBytes representative_configuration_record_for_profile(Profile);

    // Parameters that resolve to this profile and nothing more, for keying answers that depend only on it.
    static MEDIA_API Parameters canonical_parameters_for_profile(Profile);
};

}
