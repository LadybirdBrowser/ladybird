/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <AK/SeqLock.h>
#include <AK/Time.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibIPC/Forward.h>
#include <LibMedia/AudioBlockTimingRing.h>
#include <LibMedia/Export.h>

namespace Media {

enum class MediaTimeMode : u8 {
    MonotonicDriven,
    AudioDriven,
};

struct MediaTimeRecord {
    u64 epoch { 0 };
    MediaTimeMode mode { MediaTimeMode::MonotonicDriven };
    bool playing { false };
    i64 anchor_monotonic_nanoseconds { 0 };
    i64 anchor_media_time_nanoseconds { 0 };
    i64 anchor_output_frame_index { 0 };
    u32 sample_rate { 0 };
    float playback_rate { 1.0f };
};

static_assert(IsTriviallyCopyable<MediaTimeRecord>);

// The shared-memory state of one playback session's clock. Written by a single writer through
// MediaTimeWriter; read lock-free through MediaTimeReader, possibly from other
// processes.
struct MediaTimeData {
    AK_MAKE_NONCOPYABLE(MediaTimeData);
    AK_MAKE_NONMOVABLE(MediaTimeData);

public:
    MediaTimeData() = default;

    SeqLock<MediaTimeRecord> record;
    AudioBlockTimingRing timing_ring;
};

class MediaTimeWriter {
    AK_MAKE_NONCOPYABLE(MediaTimeWriter);

public:
    static ErrorOr<MediaTimeWriter> create()
    {
        auto buffer = TRY(Core::AnonymousBuffer::create_with_size(sizeof(MediaTimeData)));
        auto* data = new (buffer.data<void>()) MediaTimeData;
        return MediaTimeWriter { move(buffer), data };
    }

    MediaTimeWriter(MediaTimeWriter&&) = default;
    MediaTimeWriter& operator=(MediaTimeWriter&&) = default;

    Core::AnonymousBuffer const& buffer() const { return m_buffer; }
    AudioBlockTimingRing& timing_ring() { return m_data->timing_ring; }

    void publish_audio_anchor(MonotonicTime monotonic_time, i64 output_frame_index, AK::Duration media_time, u32 sample_rate, bool playing)
    {
        VERIFY(sample_rate > 0);
        m_data->record.write([&](TearableAtomic<MediaTimeRecord>& slot) {
            auto record = slot.load_relaxed();
            record.mode = MediaTimeMode::AudioDriven;
            record.playing = playing;
            record.anchor_monotonic_nanoseconds = monotonic_time.nanoseconds();
            record.anchor_media_time_nanoseconds = media_time.to_nanoseconds();
            record.anchor_output_frame_index = output_frame_index;
            record.sample_rate = sample_rate;
            slot.store_relaxed(record);
        });
    }

    // Re-anchors at the given output frame index, refining the anchor's media time through the
    // timing ring when it has an applicable entry.
    void refresh_audio_anchor(MonotonicTime monotonic_time, i64 output_frame_index, u32 sample_rate, bool playing)
    {
        auto media_time = AK::Duration::from_nanoseconds(m_data->record.weak_read().anchor_media_time_nanoseconds);
        auto timing = m_data->timing_ring.find_timing_for_frame_index(output_frame_index);
        if (timing.has_value())
            media_time = timing->media_time_at_frame_index(output_frame_index);
        publish_audio_anchor(monotonic_time, output_frame_index, media_time, sample_rate, playing);
    }

    void publish_monotonic_anchor(MonotonicTime monotonic_time, AK::Duration media_time, float playback_rate, bool playing)
    {
        VERIFY(isfinite(playback_rate));
        VERIFY(playback_rate >= 0.0f);
        m_data->record.write([&](TearableAtomic<MediaTimeRecord>& slot) {
            auto record = slot.load_relaxed();
            record.mode = MediaTimeMode::MonotonicDriven;
            record.playing = playing;
            record.anchor_monotonic_nanoseconds = monotonic_time.nanoseconds();
            record.anchor_media_time_nanoseconds = media_time.to_nanoseconds();
            record.playback_rate = playback_rate;
            slot.store_relaxed(record);
        });
    }

    void seek(AK::Duration target)
    {
        // Ring mutations must be visible before the new record is published, so that readers
        // never pair the new epoch with stale ring entries.
        m_data->timing_ring.clear();
        m_data->record.write([&](TearableAtomic<MediaTimeRecord>& slot) {
            auto record = slot.load_relaxed();
            record.epoch++;
            record.playing = false;
            record.anchor_media_time_nanoseconds = target.to_nanoseconds();
            if (record.mode == MediaTimeMode::AudioDriven) {
                VERIFY(record.sample_rate > 0);
                record.anchor_output_frame_index = target.to_time_units(1, record.sample_rate);
            }
            slot.store_relaxed(record);
        });
    }

private:
    MediaTimeWriter(Core::AnonymousBuffer buffer, MediaTimeData* data)
        : m_buffer(move(buffer))
        , m_data(data)
    {
    }

    Core::AnonymousBuffer m_buffer;
    MediaTimeData* m_data;
};

class MediaTimeReader {
public:
    static ErrorOr<MediaTimeReader> create(Core::AnonymousBuffer buffer)
    {
        if (!buffer.is_valid() || buffer.size() < sizeof(MediaTimeData))
            return Error::from_string_literal("Invalid playback clock buffer");
        auto const* data = reinterpret_cast<MediaTimeData const*>(buffer.data<void>());
        return MediaTimeReader { move(buffer), data };
    }

    struct TimeState {
        AK::Duration time;
        bool is_advancing { false };
    };

    AK::Duration current_time() const { return current_time(MonotonicTime::now()); }
    AK::Duration current_time(MonotonicTime now) const { return time_state(now).time; }

    // The time and whether it is advancing, from a single consistent read of the clock record.
    TimeState time_state(MonotonicTime now) const
    {
        auto maybe_record = m_data->record.read();
        if (!maybe_record.has_value())
            return { m_floor, false };
        auto const& record = maybe_record.value();

        auto elapsed_since_anchor = [&] {
            return AK::Duration::from_nanoseconds(now.nanoseconds() - record.anchor_monotonic_nanoseconds);
        };

        auto time = AK::Duration::from_nanoseconds(record.anchor_media_time_nanoseconds);
        switch (record.mode) {
        case MediaTimeMode::AudioDriven: {
            auto frame_index = record.anchor_output_frame_index;
            if (record.playing)
                frame_index += elapsed_since_anchor().to_time_units(1, record.sample_rate);

            auto timing = m_data->timing_ring.find_timing_for_frame_index(frame_index);
            if (timing.has_value())
                time = timing->media_time_at_frame_index(frame_index);
            break;
        }
        case MediaTimeMode::MonotonicDriven: {
            if (record.playing) {
                auto elapsed = elapsed_since_anchor();
                if (record.playback_rate != 1.0f)
                    elapsed = AK::Duration::from_seconds_f64(elapsed.to_seconds_f64() * record.playback_rate);
                time += elapsed;
            }
            break;
        }
        }

        if (record.epoch == m_last_epoch)
            time = max(time, m_floor);
        m_last_epoch = record.epoch;
        m_floor = time;

        auto is_advancing = record.playing;
        if (record.mode == MediaTimeMode::MonotonicDriven)
            is_advancing = is_advancing && record.playback_rate != 0.0f;
        return { time, is_advancing };
    }

    Core::AnonymousBuffer const& buffer() const { return m_buffer; }

private:
    MediaTimeReader(Core::AnonymousBuffer buffer, MediaTimeData const* data)
        : m_buffer(move(buffer))
        , m_data(data)
    {
    }

    Core::AnonymousBuffer m_buffer;
    MediaTimeData const* m_data;
    mutable u64 m_last_epoch { 0 };
    mutable AK::Duration m_floor;
};

}

namespace IPC {

template<>
MEDIA_API ErrorOr<void> encode(Encoder&, Media::MediaTimeReader const&);

template<>
MEDIA_API ErrorOr<Media::MediaTimeReader> decode(Decoder&);

}
