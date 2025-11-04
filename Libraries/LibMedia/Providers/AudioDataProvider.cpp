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
        while (!thread_data->should_thread_exit()) {
            thread_data->handle_seek();
            thread_data->push_data_and_decode_a_block();
        }
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

void AudioDataProvider::seek(AK::Duration timestamp, SeekCompletionHandler&& completion_handler)
{
    m_thread_data->seek(timestamp, move(completion_handler));
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

void AudioDataProvider::ThreadData::seek(AK::Duration timestamp, SeekCompletionHandler&& completion_handler)
{
    auto locker = take_lock();
    m_seek_completion_handler = move(completion_handler);
    m_seek_id++;
    m_seek_timestamp = timestamp;
    m_wait_condition.broadcast();
}

bool AudioDataProvider::ThreadData::should_thread_exit() const
{
    return m_exit;
}

template<typename T>
void AudioDataProvider::ThreadData::process_seek_on_main_thread(u32 seek_id, T&& function)
{
    m_last_processed_seek_id = seek_id;
    m_main_thread_event_loop.deferred_invoke([this, seek_id, function] mutable {
        if (m_seek_id != seek_id)
            return;
        function();
    });
}

void AudioDataProvider::ThreadData::resolve_seek(u32 seek_id)
{
    process_seek_on_main_thread(seek_id, [this] {
        {
            auto locker = take_lock();
            m_is_in_error_state = false;
            m_wait_condition.broadcast();
        }
        auto handler = move(m_seek_completion_handler);
        if (handler)
            handler();
    });
}

bool AudioDataProvider::ThreadData::handle_seek()
{
    auto seek_id = m_seek_id.load();
    if (m_last_processed_seek_id == seek_id)
        return false;

    auto handle_error = [&](DecoderError&& error) {
        {
            auto locker = take_lock();
            m_queue.clear();
        }

        process_seek_on_main_thread(seek_id,
            [this, error = move(error)] mutable {
                m_error_handler(move(error));
                m_seek_completion_handler = nullptr;
            });
    };

    AK::Duration timestamp;

    while (true) {
        {
            auto locker = take_lock();
            seek_id = m_seek_id;
            timestamp = m_seek_timestamp;
        }

        auto demuxer_seek_result_or_error = m_demuxer->seek_to_most_recent_keyframe(m_track, timestamp);
        if (demuxer_seek_result_or_error.is_error() && demuxer_seek_result_or_error.error().category() != DecoderErrorCategory::EndOfStream) {
            handle_error(demuxer_seek_result_or_error.release_error());
            return true;
        }
        auto demuxer_seek_result = demuxer_seek_result_or_error.value_or(DemuxerSeekResult::MovedPosition);

        if (demuxer_seek_result == DemuxerSeekResult::MovedPosition)
            m_decoder->flush();

        auto new_seek_id = seek_id;
        AudioBlock last_block;

        while (new_seek_id == seek_id) {
            auto coded_frame_result = m_demuxer->get_next_sample_for_track(m_track);
            if (coded_frame_result.is_error()) {
                if (coded_frame_result.error().category() == DecoderErrorCategory::EndOfStream) {
                    m_decoder->signal_end_of_stream();
                } else {
                    handle_error(coded_frame_result.release_error());
                    return true;
                }
            } else {
                auto coded_frame = coded_frame_result.release_value();
                auto decode_result = m_decoder->receive_coded_data(coded_frame.timestamp(), coded_frame.data());
                if (decode_result.is_error()) {
                    handle_error(decode_result.release_error());
                    return true;
                }
            }

            while (new_seek_id == seek_id) {
                AudioBlock current_block;
                auto block_result = m_decoder->write_next_block(current_block);
                if (block_result.is_error()) {
                    if (block_result.error().category() == DecoderErrorCategory::EndOfStream) {
                        resolve_seek(seek_id);
                        return true;
                    }

                    if (block_result.error().category() == DecoderErrorCategory::NeedsMoreInput)
                        break;

                    handle_error(block_result.release_error());
                    return true;
                }

                if (current_block.start_timestamp() > timestamp) {
                    auto locker = take_lock();
                    m_queue.clear();

                    if (!last_block.is_empty())
                        m_queue.enqueue(move(last_block));

                    m_queue.enqueue(move(current_block));

                    resolve_seek(seek_id);
                    return true;
                }

                last_block = move(current_block);

                new_seek_id = m_seek_id;
            }
        }
    }
}

void AudioDataProvider::ThreadData::push_data_and_decode_a_block()
{
    auto set_error_and_wait_for_seek = [this](DecoderError&& error) {
        auto is_in_error_state = true;

        {
            auto locker = take_lock();
            m_is_in_error_state = true;
            while (!m_error_handler)
                m_wait_condition.wait();
            m_main_thread_event_loop.deferred_invoke([this, error = move(error)] mutable {
                m_error_handler(move(error));
            });
        }

        dbgln_if(PLAYBACK_MANAGER_DEBUG, "Audio Data Provider: Encountered an error, waiting for a seek to start decoding again...");
        while (is_in_error_state) {
            if (handle_seek())
                break;

            {
                auto locker = take_lock();
                m_wait_condition.wait();
                is_in_error_state = m_is_in_error_state;
            }
        }
    };

    auto sample_result = m_demuxer->get_next_sample_for_track(m_track);
    if (sample_result.is_error()) {
        if (sample_result.error().category() == DecoderErrorCategory::EndOfStream) {
            m_decoder->signal_end_of_stream();
        } else {
            set_error_and_wait_for_seek(sample_result.release_error());
            return;
        }
    } else {
        auto sample = sample_result.release_value();
        auto decode_result = m_decoder->receive_coded_data(sample.timestamp(), sample.data());
        if (decode_result.is_error()) {
            set_error_and_wait_for_seek(decode_result.release_error());
            return;
        }
    }

    while (true) {
        auto queue_size = [&] {
            auto locker = take_lock();
            return m_queue.size();
        }();

        while (queue_size >= m_queue_max_size) {
            if (handle_seek())
                return;

            {
                auto locker = take_lock();
                m_wait_condition.wait();
                if (should_thread_exit())
                    return;
                queue_size = m_queue.size();
            }
        }

        auto block = AudioBlock();
        auto timestamp_result = m_decoder->write_next_block(block);
        if (timestamp_result.is_error()) {
            if (timestamp_result.error().category() == DecoderErrorCategory::NeedsMoreInput)
                break;
            set_error_and_wait_for_seek(timestamp_result.release_error());
            break;
        }

        // FIXME: Specify trailing samples in the demuxer, and drop them here or in the audio decoder implementation.

        auto locker = take_lock();
        VERIFY(!block.is_empty());
        m_queue.enqueue(move(block));
        VERIFY(!m_queue.tail().is_empty());
    }
}

}
