/*
 * Copyright (c) 2022-2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Time.h>
#include <LibMedia/CodedAudioFrameData.h>
#include <LibMedia/CodedVideoFrameData.h>
#include <LibMedia/FrameFlags.h>

namespace Media {

class CodedFrame final {
public:
    using AuxiliaryData = Variant<CodedVideoFrameData, CodedAudioFrameData>;

    CodedFrame(AK::Duration timestamp, AK::Duration duration, FrameFlags flags, ByteBuffer&& data, AuxiliaryData auxiliary_data)
        : m_timestamp(timestamp)
        , m_duration(duration)
        , m_flags(flags)
        , m_data(move(data))
        , m_auxiliary_data(auxiliary_data)
    {
    }

    AK::Duration timestamp() const { return m_timestamp; }
    AK::Duration duration() const { return m_duration; }
    FrameFlags flags() const { return m_flags; }
    bool is_keyframe() const { return has_flag(m_flags, FrameFlags::Keyframe); }
    ByteBuffer const& data() const { return m_data; }
    AuxiliaryData const& auxiliary_data() const { return m_auxiliary_data; }

private:
    AK::Duration m_timestamp;
    AK::Duration m_duration;
    FrameFlags m_flags;
    ByteBuffer m_data;
    AuxiliaryData m_auxiliary_data;
};

}
