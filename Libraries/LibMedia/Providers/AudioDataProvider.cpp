/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibCore/EventLoop.h>
#include <LibMedia/FFmpeg/FFmpegAudioDecoder.h>
#include <LibMedia/MutexedDemuxer.h>
#include <LibMedia/Sinks/AudioSink.h>
#include <LibThreading/Mutex.h>
#include <LibThreading/Thread.h>

#include "AudioDataProvider.h"

namespace Media {

DecoderErrorOr<NonnullRefPtr<AudioDataProvider>> AudioDataProvider::try_create(NonnullRefPtr<MutexedDemuxer> const& demuxer, Track const& track)
{
    auto codec_id = TRY(demuxer->get_codec_id_for_track(track));
    auto codec_initialization_data = TRY(demuxer->get_codec_initialization_data_for_track(track));
    auto decoder = DECODER_TRY_ALLOC(FFmpeg::FFmpegAudioDecoder::try_create(codec_id, codec_initialization_data));

    auto thread_data = DECODER_TRY_ALLOC(try_make_ref_counted<AudioDataProvider::ThreadData>(demuxer, track, move(decoder)));
    auto provider = DECODER_TRY_ALLOC(try_make_ref_counted<AudioDataProvider>(thread_data));

    auto thread = DECODER_TRY_ALLOC(Threading::Thread::try_create([thread_data]() -> int {
        while (!thread_data->should_thread_exit())
            thread_data->push_data_and_decode_a_block();
        return 0;
    }));
    thread->start();
    thread->detach();

    return provider;
}

AudioDataProvider::AudioDataProvider(NonnullRefPtr<ThreadData> const& thread_data)
    : m_thread_data(thread_data)
{
}

AudioDataProvider::~AudioDataProvider()
{
    m_thread_data->exit();
}

void AudioDataProvider::set_error_handler(ErrorHandler&& handler)
{
    m_thread_data->set_error_handler(move(handler));
}

void AudioDataProvider::seek(AK::Duration timestamp)
{
    m_thread_data->seek(timestamp);
}

AudioDataProvider::ThreadData::ThreadData(NonnullRefPtr<MutexedDemuxer> const& demuxer, Track const& track, NonnullOwnPtr<AudioDecoder>&& decoder)
    : m_main_thread_event_loop(Core::EventLoop::current())
    , m_demuxer(demuxer)
    , m_track(track)
    , m_decoder(move(decoder))
{
}

AudioDataProvider::ThreadData::~ThreadData() = default;

void AudioDataProvider::ThreadData::set_error_handler(ErrorHandler&& handler)
{
    auto locker = take_lock();
    m_error_handler = move(handler);
    m_wait_condition.broadcast();
}

AudioBlock AudioDataProvider::retrieve_block()
{
    auto locker = m_thread_data->take_lock();
    if (m_thread_data->queue().is_empty())
        return AudioBlock();
    auto result = m_thread_data->queue().dequeue();
    m_thread_data->wake();
    return result;
}

void AudioDataProvider::ThreadData::exit()
{
    m_exit = true;
    m_wait_condition.broadcast();
}

void AudioDataProvider::ThreadData::seek(AK::Duration timestamp)
{
    auto demuxer_result = m_demuxer->seek_to_most_recent_keyframe(m_track, timestamp);
    if (demuxer_result.is_error()) {
        m_error_handler(demuxer_result.release_error());
    } else {
        auto locker = take_lock();
        m_is_in_error_state = false;
        m_wait_condition.broadcast();
    }
}

bool AudioDataProvider::ThreadData::should_thread_exit() const
{
    return m_exit;
}

void AudioDataProvider::ThreadData::push_data_and_decode_a_block()
{
#if PLAYBACK_MANAGER_DEBUG
    auto start_time = MonotonicTime::now();
#endif

    auto set_error_and_wait_for_seek = [this](DecoderError&& error) {
        auto locker = take_lock();
        m_is_in_error_state = true;
        while (!m_error_handler)
            m_wait_condition.wait();
        m_main_thread_event_loop.deferred_invoke([this, error = move(error)] mutable {
            m_error_handler(move(error));
        });
        dbgln_if(PLAYBACK_MANAGER_DEBUG, "Audio Data Provider: Encountered an error, waiting for a seek to start decoding again...");
        while (m_is_in_error_state)
            m_wait_condition.wait();
    };

    auto sample_result = m_demuxer->get_next_sample_for_track(m_track);
    if (sample_result.is_error()) {
        if (sample_result.error().category() == DecoderErrorCategory::NeedsMoreInput) {
            return;
        }
        // FIXME: Handle the end of the stream.
        set_error_and_wait_for_seek(sample_result.release_error());
        return;
    }

    auto sample = sample_result.release_value();
    auto decode_result = m_decoder->receive_coded_data(sample.timestamp(), sample.data());
    if (decode_result.is_error()) {
        set_error_and_wait_for_seek(decode_result.release_error());
        return;
    }

    while (true) {
        auto locker = take_lock();

        while (m_queue.size() >= m_queue_max_size) {
            m_wait_condition.wait();
            if (should_thread_exit())
                return;
        }

        auto block = AudioBlock();
        auto timestamp_result = m_decoder->write_next_block(block);
        if (timestamp_result.is_error()) {
            if (timestamp_result.error().category() == DecoderErrorCategory::NeedsMoreInput)
                break;
            set_error_and_wait_for_seek(timestamp_result.release_error());
            break;
        }

        VERIFY(!block.is_empty());
        m_queue.enqueue(move(block));
        VERIFY(!m_queue.tail().is_empty());
    }
}

}
