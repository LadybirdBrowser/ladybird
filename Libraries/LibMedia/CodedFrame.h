/*
 * Copyright (c) 2022-2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FixedArray.h>
#include <AK/Optional.h>
#include <AK/Time.h>
#include <LibMedia/CodecID.h>
#include <LibMedia/FrameFlags.h>

namespace Media {

class CodedFrame final {
public:
    CodedFrame(CodecID codec_id, AK::Duration presentation_timestamp, AK::Duration decode_timestamp, AK::Duration duration, FrameFlags flags, FixedArray<u8>&& data, Optional<FixedArray<u8>> new_codec_configuration = {})
        : m_codec_id(codec_id)
        , m_flags(flags)
        , m_has_new_codec_configuration(new_codec_configuration.has_value())
        , m_presentation_timestamp(presentation_timestamp)
        , m_decode_timestamp(decode_timestamp)
        , m_duration(duration)
        , m_data(move(data))
        , m_new_codec_configuration(new_codec_configuration.has_value() ? new_codec_configuration.release_value() : FixedArray<u8> {})
    {
    }

    CodedFrame(CodedFrame const& other)
        : CodedFrame(other.m_codec_id, other.m_presentation_timestamp, other.m_decode_timestamp, other.m_duration, other.m_flags, MUST(other.m_data.clone()), other.new_codec_configuration_storage())
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

    CodecID codec_id() const { return m_codec_id; }
    AK::Duration presentation_timestamp() const { return m_presentation_timestamp; }
    void set_presentation_timestamp(AK::Duration timestamp) { m_presentation_timestamp = timestamp; }
    AK::Duration decode_timestamp() const { return m_decode_timestamp; }
    void set_decode_timestamp(AK::Duration timestamp) { m_decode_timestamp = timestamp; }
    AK::Duration duration() const { return m_duration; }
    FrameFlags flags() const { return m_flags; }
    bool is_keyframe() const { return has_flag(m_flags, FrameFlags::Keyframe); }
    ReadonlyBytes data() const LIFETIME_BOUND { return m_data.span(); }
    Optional<ReadonlyBytes> new_codec_configuration() const LIFETIME_BOUND
    {
        if (!m_has_new_codec_configuration)
            return {};
        return m_new_codec_configuration.span();
    }

    void set_new_codec_configuration(FixedArray<u8> new_codec_configuration)
    {
        m_new_codec_configuration = move(new_codec_configuration);
        m_has_new_codec_configuration = true;
    }

private:
    Optional<FixedArray<u8>> new_codec_configuration_storage() const
    {
        if (!m_has_new_codec_configuration)
            return {};
        return MUST(m_new_codec_configuration.clone());
    }

    CodecID m_codec_id;
    FrameFlags m_flags;
    bool m_has_new_codec_configuration { false };
    AK::Duration m_presentation_timestamp;
    AK::Duration m_decode_timestamp;
    AK::Duration m_duration;
    FixedArray<u8> m_data;
    FixedArray<u8> m_new_codec_configuration;
};

}
