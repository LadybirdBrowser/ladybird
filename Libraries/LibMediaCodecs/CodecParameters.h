/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashFunctions.h>
#include <AK/Optional.h>
#include <AK/StdLibExtras.h>
#include <AK/StringView.h>
#include <AK/Traits.h>
#include <LibIPC/Forward.h>
#include <LibMediaCodecs/CodecID.h>
#include <LibMediaCodecs/Codecs/AAC.h>
#include <LibMediaCodecs/Codecs/AV1.h>
#include <LibMediaCodecs/Codecs/H264.h>
#include <LibMediaCodecs/Codecs/H265.h>
#include <LibMediaCodecs/Codecs/VP9.h>
#include <LibMediaCodecs/Export.h>

namespace Media {

class CodedFrame;

class ParsedCodec {
public:
    constexpr explicit ParsedCodec(CodecID codec_id)
        : m_codec_id(codec_id)
    {
    }

    constexpr explicit ParsedCodec(Codecs::AAC::Parameters parameters)
        : m_codec_id(CodecID::AAC)
        , m_parameters(parameters)
    {
    }

    constexpr explicit ParsedCodec(Codecs::H264::Parameters parameters)
        : m_codec_id(CodecID::H264)
        , m_parameters(parameters)
    {
    }

    constexpr explicit ParsedCodec(Codecs::H265::Parameters parameters)
        : m_codec_id(CodecID::H265)
        , m_parameters(parameters)
    {
    }

    constexpr explicit ParsedCodec(Codecs::AV1::Parameters parameters)
        : m_codec_id(CodecID::AV1)
        , m_parameters(parameters)
    {
    }

    constexpr explicit ParsedCodec(Codecs::VP9::Parameters parameters)
        : m_codec_id(CodecID::VP9)
        , m_parameters(parameters)
    {
    }

    constexpr CodecID codec_id() const { return m_codec_id; }
    constexpr bool has_parameters() const { return m_parameters.has_value(); }

    unsigned hash() const
    {
        auto combine_hashes = [](unsigned hash, auto... values) {
            ((hash = pair_int_hash(hash, static_cast<u32>(values))), ...);
            return hash;
        };
        auto combine_cicp_hash = [&](unsigned hash, CodingIndependentCodePoints const& cicp) {
            hash = combine_hashes(hash, to_underlying(cicp.color_primaries()), to_underlying(cicp.transfer_characteristics()));
            return combine_hashes(hash, to_underlying(cicp.matrix_coefficients()), to_underlying(cicp.video_full_range_flag()));
        };

        auto hash = u32_hash(to_underlying(m_codec_id));
        if (!m_parameters.has_value())
            return hash;

        switch (m_codec_id) {
        case CodecID::AAC: {
            auto const& parameters = m_parameters->aac;
            hash = combine_hashes(hash, parameters.object_type_indication, parameters.audio_object_type.has_value());
            return combine_hashes(hash, parameters.audio_object_type.value_or(0));
        }
        case CodecID::AV1: {
            auto const& parameters = m_parameters->av1;
            hash = combine_hashes(hash, parameters.profile, parameters.level, to_underlying(parameters.tier), parameters.bit_depth);
            auto const& optional_fields = parameters.optional_fields;
            hash = combine_hashes(hash, optional_fields.monochrome, optional_fields.chroma_sample_position);
            hash = combine_hashes(hash, optional_fields.subsampling.x(), optional_fields.subsampling.y());
            return combine_cicp_hash(hash, optional_fields.cicp);
        }
        case CodecID::H264: {
            auto const& parameters = m_parameters->h264;
            return combine_hashes(hash, parameters.profile_idc, parameters.constraint_set_flags, parameters.level_idc);
        }
        case CodecID::H265: {
            auto const& parameters = m_parameters->h265;
            hash = combine_hashes(hash, parameters.profile_space, parameters.profile_idc, parameters.profile_compatibility_flags);
            hash = combine_hashes(hash, parameters.level_idc, parameters.tier_flag);
            for (auto flags : parameters.constraint_indicator_flags)
                hash = combine_hashes(hash, flags);
            return hash;
        }
        case CodecID::VP9: {
            auto const& parameters = m_parameters->vp9;
            hash = combine_hashes(hash, parameters.profile, parameters.level, parameters.bit_depth);
            auto const& color_parameters = parameters.color_parameters;
            hash = combine_hashes(hash, color_parameters.subsampling.x(), color_parameters.subsampling.y());
            return combine_cicp_hash(hash, color_parameters.cicp);
        }
        default:
            return hash;
        }
    }

    constexpr bool operator==(ParsedCodec const& other) const
    {
        if (m_codec_id != other.m_codec_id || m_parameters.has_value() != other.m_parameters.has_value())
            return false;
        if (!m_parameters.has_value())
            return true;

        switch (m_codec_id) {
        case CodecID::AAC:
            return m_parameters->aac == other.m_parameters->aac;
        case CodecID::AV1:
            return m_parameters->av1 == other.m_parameters->av1;
        case CodecID::H264:
            return m_parameters->h264 == other.m_parameters->h264;
        case CodecID::H265:
            return m_parameters->h265 == other.m_parameters->h265;
        case CodecID::VP9:
            return m_parameters->vp9 == other.m_parameters->vp9;
        default:
            return true;
        }
    }

    constexpr bool is_fully_specified() const
    {
        switch (m_codec_id) {
        case CodecID::AAC:
            if (!m_parameters.has_value())
                return false;
            return m_parameters->aac.is_fully_specified();
        case CodecID::AV1:
        case CodecID::H264:
        case CodecID::H265:
            return m_parameters.has_value();
        case CodecID::VP9:
        default:
            return true;
        }
    }

    constexpr Optional<CodingIndependentCodePoints> color_information() const
    {
        if (!m_parameters.has_value())
            return {};
        switch (m_codec_id) {
        case CodecID::AV1:
            return m_parameters->av1.optional_fields.cicp;
        case CodecID::VP9:
            return m_parameters->vp9.color_parameters.cicp;
        default:
            return {};
        }
    }

    constexpr Optional<Codecs::AAC::Parameters const&> aac_parameters() const
    {
        VERIFY(m_codec_id == CodecID::AAC);
        if (!m_parameters.has_value())
            return {};
        return m_parameters->aac;
    }

    constexpr Optional<Codecs::H264::Parameters const&> h264_parameters() const
    {
        VERIFY(m_codec_id == CodecID::H264);
        if (!m_parameters.has_value())
            return {};
        return m_parameters->h264;
    }

    constexpr Optional<Codecs::H265::Parameters const&> h265_parameters() const
    {
        VERIFY(m_codec_id == CodecID::H265);
        if (!m_parameters.has_value())
            return {};
        return m_parameters->h265;
    }

    constexpr Optional<Codecs::AV1::Parameters const&> av1_parameters() const
    {
        VERIFY(m_codec_id == CodecID::AV1);
        if (!m_parameters.has_value())
            return {};
        return m_parameters->av1;
    }

    constexpr Optional<Codecs::VP9::Parameters const&> vp9_parameters() const
    {
        VERIFY(m_codec_id == CodecID::VP9);
        if (!m_parameters.has_value())
            return {};
        return m_parameters->vp9;
    }

private:
    union Parameters {
        constexpr Parameters(Codecs::AAC::Parameters parameters)
            : aac(parameters)
        {
        }

        constexpr Parameters(Codecs::H264::Parameters parameters)
            : h264(parameters)
        {
        }

        constexpr Parameters(Codecs::H265::Parameters parameters)
            : h265(parameters)
        {
        }

        constexpr Parameters(Codecs::AV1::Parameters parameters)
            : av1(parameters)
        {
        }

        constexpr Parameters(Codecs::VP9::Parameters parameters)
            : vp9(parameters)
        {
        }

        Codecs::AAC::Parameters aac;
        Codecs::AV1::Parameters av1;
        Codecs::H264::Parameters h264;
        Codecs::H265::Parameters h265;
        Codecs::VP9::Parameters vp9;
    };

    static_assert(IsTriviallyCopyable<Parameters>);

    CodecID m_codec_id;
    Optional<Parameters> m_parameters;
};

static_assert(IsTriviallyCopyable<ParsedCodec>);

MEDIACODECS_API Optional<ParsedCodec> parse_codec_parameters_string(StringView);

MEDIACODECS_API ParsedCodec parsed_codec_for_coded_frame(CodedFrame const&, ParsedCodec const& container_codec);

// Keeps only what decides whether a decoder supports a format, dropping level, tier and colour, so that formats a
// decoder handles alike share one key.
MEDIACODECS_API ParsedCodec normalize_parsed_codec_for_support_keying(ParsedCodec const&);

}

namespace AK {

template<>
struct Traits<Media::ParsedCodec> : public DefaultTraits<Media::ParsedCodec> {
    static unsigned hash(Media::ParsedCodec const& codec) { return codec.hash(); }
};

}

namespace IPC {

template<>
MEDIACODECS_API ErrorOr<void> encode(Encoder&, Media::ParsedCodec const&);

template<>
MEDIACODECS_API ErrorOr<Media::ParsedCodec> decode(Decoder&);

}
