/*
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/HashFunctions.h>
#include <AK/Time.h>
#include <AK/Traits.h>
#include <AK/Types.h>
#include <AK/Utf16String.h>
#include <AK/Variant.h>
#include <LibMedia/Audio/SampleSpecification.h>
#include <LibMedia/Color/CodingIndependentCodePoints.h>
#include <LibMedia/TrackType.h>

namespace Media {

class Track {
public:
    struct VideoData {
        u64 pixel_width { 0 };
        u64 pixel_height { 0 };
        CodingIndependentCodePoints cicp;
    };

    struct AudioData {
        Audio::SampleSpecification sample_specification;
    };

    // Derived from the "kind" attributes in:
    // https://dev.w3.org/html5/html-sourcing-inband-tracks/
    enum class Kind : u8 {
        None,
        Alternative,
        Captions,
        Descriptions,
        Main,
        MainDesc,
        Metadata,
        Sign,
        Subtitles,
        Translation,
        Commentary,
    };

    Track(TrackType type, size_t identifier, Kind kind, Utf16String const& label, Utf16String const& language)
        : m_type(type)
        , m_identifier(identifier)
        , m_kind(kind)
        , m_label(label)
        , m_language(language)
    {
        switch (m_type) {
        case TrackType::Video:
            m_track_data = VideoData {};
            break;
        case TrackType::Audio:
            m_track_data = AudioData {};
            break;
        default:
            m_track_data = Empty {};
            break;
        }
    }

    TrackType type() const { return m_type; }
    size_t identifier() const { return m_identifier; }
    Kind kind() const { return m_kind; }
    Utf16String const& label() const { return m_label; }
    Utf16String const& language() const { return m_language; }

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

    void set_audio_data(AudioData data)
    {
        VERIFY(m_type == TrackType::Audio);
        m_track_data = data;
    }

    AudioData const& audio_data() const
    {
        VERIFY(m_type == TrackType::Audio);
        return m_track_data.get<AudioData>();
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
    TrackType m_type { 0 };
    size_t m_identifier { 0 };
    Kind m_kind { Kind::None };
    Utf16String m_label;
    Utf16String m_language;

    Variant<Empty, VideoData, AudioData> m_track_data;
};

constexpr Utf16View track_kind_to_string(Track::Kind kind)
{
    switch (kind) {
    case Track::Kind::None:
        return u""sv;
    case Track::Kind::Alternative:
        return u"alternative"sv;
    case Track::Kind::Captions:
        return u"captions"sv;
    case Track::Kind::Descriptions:
        return u"descriptions"sv;
    case Track::Kind::Main:
        return u"main"sv;
    case Track::Kind::MainDesc:
        return u"maindesc"sv;
    case Track::Kind::Metadata:
        return u"metadata"sv;
    case Track::Kind::Sign:
        return u"sign"sv;
    case Track::Kind::Subtitles:
        return u"subtitles"sv;
    case Track::Kind::Translation:
        return u"translation"sv;
    case Track::Kind::Commentary:
        return u"commentary"sv;
    }
    VERIFY_NOT_REACHED();
}

}

template<>
struct AK::Traits<Media::Track> : public DefaultTraits<Media::Track> {
    static unsigned hash(Media::Track const& t) { return t.hash(); }
};
