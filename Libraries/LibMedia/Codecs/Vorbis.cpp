/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibMedia/Codecs/Vorbis.h>

extern "C" {
#include <libavcodec/vorbis_parser.h>
}

namespace Media::Codecs {

Optional<Vorbis::Parser> Vorbis::Parser::create(ReadonlyBytes codec_initialization_data)
{
    if (codec_initialization_data.is_empty())
        return {};

    auto* parser = av_vorbis_parse_init(codec_initialization_data.data(), AK::clamp_to<int>(codec_initialization_data.size()));
    if (!parser)
        return {};
    return Parser { parser };
}

Vorbis::Parser::Parser(AVVorbisParseContext* context)
    : m_context(context)
{
    VERIFY(m_context);
}

Vorbis::Parser::Parser(Parser&& other)
    : m_context(exchange(other.m_context, nullptr))
{
}

Vorbis::Parser& Vorbis::Parser::operator=(Parser&& other)
{
    if (this == &other)
        return *this;

    if (m_context)
        av_vorbis_parse_free(&m_context);
    m_context = exchange(other.m_context, nullptr);
    return *this;
}

Vorbis::Parser::~Parser()
{
    if (m_context)
        av_vorbis_parse_free(&m_context);
}

void Vorbis::Parser::reset() const
{
    av_vorbis_parse_reset(m_context);
}

Optional<u32> Vorbis::Parser::parse_packet_duration_in_samples(ReadonlyBytes packet) const
{
    int flags = 0;
    auto duration = av_vorbis_parse_frame_flags(m_context, packet.data(), AK::clamp_to<int>(packet.size()), &flags);
    if (duration < 0)
        return {};
    return duration;
}

Optional<AK::Duration> Vorbis::Parser::parse_packet_duration(ReadonlyBytes packet, u32 sample_rate) const
{
    auto duration = parse_packet_duration_in_samples(packet);
    if (!duration.has_value())
        return {};
    return AK::Duration::from_time_units(duration.value(), 1, sample_rate);
}

}
