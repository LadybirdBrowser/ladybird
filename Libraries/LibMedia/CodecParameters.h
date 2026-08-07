/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/StdLibExtras.h>
#include <AK/StringView.h>
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
