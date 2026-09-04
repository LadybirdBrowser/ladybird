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
#include <LibMedia/CodecID.h>
#include <LibMedia/Codecs/AAC.h>
#include <LibMedia/Codecs/AV1.h>
#include <LibMedia/Codecs/H264.h>
#include <LibMedia/Codecs/H265.h>
#include <LibMedia/Codecs/VP9.h>
#include <LibMedia/Export.h>

namespace Media {

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

    constexpr unsigned hash() const
    {
        auto codec_hash = u32_hash(to_underlying(m_codec_id));
        if (!m_parameters.has_value())
            return codec_hash;

        switch (m_codec_id) {
        case CodecID::AV1:
            return pair_int_hash(codec_hash, pair_int_hash(m_parameters->av1.profile, m_parameters->av1.bit_depth));
        case CodecID::H264:
            return pair_int_hash(codec_hash, pair_int_hash(m_parameters->h264.profile_idc, m_parameters->h264.level_idc));
        case CodecID::H265:
            return pair_int_hash(codec_hash, pair_int_hash(m_parameters->h265.profile_idc, m_parameters->h265.level_idc));
        case CodecID::VP9:
            return pair_int_hash(codec_hash, pair_int_hash(m_parameters->vp9.profile, m_parameters->vp9.bit_depth));
        default:
            return codec_hash;
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

MEDIA_API Optional<ParsedCodec> parse_codec_parameters_string(StringView);

}

namespace AK {

template<>
struct Traits<Media::ParsedCodec> : public DefaultTraits<Media::ParsedCodec> {
    static unsigned hash(Media::ParsedCodec const& codec) { return codec.hash(); }
};

}
