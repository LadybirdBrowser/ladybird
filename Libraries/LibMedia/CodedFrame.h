/*
 * Copyright (c) 2022-2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FixedArray.h>
#include <AK/Time.h>
#include <LibMedia/FrameFlags.h>

namespace Media {

class CodedFrame final {
public:
    CodedFrame(AK::Duration presentation_timestamp, AK::Duration decode_timestamp, AK::Duration duration, FrameFlags flags, FixedArray<u8>&& data, FixedArray<u8> new_codec_configuration = {})
        : m_presentation_timestamp(presentation_timestamp)
        , m_decode_timestamp(decode_timestamp)
        , m_duration(duration)
        , m_flags(flags)
        , m_data(move(data))
        , m_new_codec_configuration(move(new_codec_configuration))
    {
    }

    CodedFrame(CodedFrame const& other)
        : CodedFrame(other.m_presentation_timestamp, other.m_decode_timestamp, other.m_duration, other.m_flags, MUST(other.m_data.clone()), MUST(other.m_new_codec_configuration.clone()))
    {
    }

    CodedFrame(CodedFrame&&) = default;

    CodedFrame& operator=(CodedFrame const& other)
    {
        if (this != &other)
            *this = CodedFrame(other);
        return *this;
    }

    CodedFrame& operator=(CodedFrame&&) = default;

    AK::Duration presentation_timestamp() const { return m_presentation_timestamp; }
    AK::Duration decode_timestamp() const { return m_decode_timestamp; }
    AK::Duration duration() const { return m_duration; }
    FrameFlags flags() const { return m_flags; }
    bool is_keyframe() const { return has_flag(m_flags, FrameFlags::Keyframe); }
    ReadonlyBytes data() const LIFETIME_BOUND { return m_data.span(); }
    ReadonlyBytes new_codec_configuration() const LIFETIME_BOUND { return m_new_codec_configuration.span(); }
    void set_new_codec_configuration(FixedArray<u8> new_codec_configuration) { m_new_codec_configuration = move(new_codec_configuration); }

private:
    AK::Duration m_presentation_timestamp;
    AK::Duration m_decode_timestamp;
    AK::Duration m_duration;
    FrameFlags m_flags;
    FixedArray<u8> m_data;
    FixedArray<u8> m_new_codec_configuration;
};

}
