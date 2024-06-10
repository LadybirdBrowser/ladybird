/*
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashFunctions.h>
#include <AK/Time.h>
#include <AK/Traits.h>
#include <AK/Types.h>
#include <AK/Variant.h>

namespace Media {

enum class TrackType : u32 {
    Video,
    Audio,
    Subtitles,
};

class Track {
    struct VideoData {
        Duration duration {};
        u64 pixel_width { 0 };
        u64 pixel_height { 0 };
    };

public:
    Track(TrackType type, size_t identifier)
        : m_type(type)
        , m_identifier(identifier)
    {
        switch (m_type) {
        case TrackType::Video:
            m_track_data = VideoData {};
            break;
        default:
            m_track_data = Empty {};
            break;
        }
    }

    TrackType type() { return m_type; }
    size_t identifier() const { return m_identifier; }

    void set_video_data(VideoData data)
    {
        VERIFY(m_type == TrackType::Video);
        m_track_data = data;
    }

    VideoData const& video_data() const
    {
        VERIFY(m_type == TrackType::Video);
        return m_track_data.get<VideoData>();
    }

    bool operator==(Track const& other) const
    {
        return m_type == other.m_type && m_identifier == other.m_identifier;
    }

    unsigned hash() const
    {
        return pair_int_hash(to_underlying(m_type), m_identifier);
    }

private:
    TrackType m_type;
    size_t m_identifier;

    Variant<Empty, VideoData> m_track_data;
};

}

template<>
struct AK::Traits<Media::Track> : public DefaultTraits<Media::Track> {
    static unsigned hash(Media::Track const& t) { return t.hash(); }
};
