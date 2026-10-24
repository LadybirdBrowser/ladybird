/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/ConditionVariable.h>
#include <AK/Function.h>
#include <AK/Mutex.h>
#include <AK/NeverDestroyed.h>
#include <AK/Vector.h>
#include <LibMediaCodecs/CodecID.h>
#include <LibMediaCodecs/CodedFrame.h>
#include <LibMediaCodecs/VideoDecoder.h>
#include <LibMediaCodecs/VideoDecoderRegistry.h>
#include <LibMediaCodecs/VideoFrame.h>
#include <LibMediaCodecs/VideoFramePool.h>

// The outputs that a WithholdingVideoDecoder has finished but not handed out yet. A test releases them one at a time,
// from whichever thread it likes, the way a decoder running on threads of its own finishes frames.
class WithheldOutputs final : public AtomicRefCounted<WithheldOutputs> {
public:
    struct Output {
        AK::Duration timestamp;
        AK::Duration duration;
    };

    bool release_next()
    {
        MutexLocker locker { m_mutex };
        if (m_withheld.is_empty())
            return false;
        m_released.append(m_withheld.take_first());
        m_released_changed.broadcast();
        if (m_output_arrived_callback && !m_caller_is_waiting_for_output)
            m_output_arrived_callback();
        return true;
    }

    // The decoder reports that it lost its state once, instead of handing out the output at this index.
    void lose_state_instead_of_handing_out_output(size_t output_index)
    {
        MutexLocker locker { m_mutex };
        m_output_index_that_loses_state = output_index;
    }

    // The decoder reports that it lost its state once, instead of reporting the end of the stream.
    void lose_state_at_end_of_stream()
    {
        MutexLocker locker { m_mutex };
        m_loses_state_at_end_of_stream = true;
    }

    bool state_was_lost() const
    {
        MutexLocker locker { m_mutex };
        return m_state_was_lost;
    }

    size_t withheld_count() const
    {
        MutexLocker locker { m_mutex };
        return m_withheld.size();
    }

    bool wait_until_withheld_count_reaches(size_t count, AK::Duration timeout)
    {
        auto deadline = MonotonicTime::now() + timeout;
        MutexLocker locker { m_mutex };
        while (m_withheld.size() < count) {
            auto now = MonotonicTime::now();
            if (now >= deadline)
                return false;
            m_withheld_changed.wait_for(deadline - now);
        }
        return true;
    }

    bool wait_until_the_caller_waits_for_output(AK::Duration timeout)
    {
        auto deadline = MonotonicTime::now() + timeout;
        MutexLocker locker { m_mutex };
        while (!m_caller_is_waiting_for_output) {
            auto now = MonotonicTime::now();
            if (now >= deadline)
                return false;
            m_caller_wait_changed.wait_for(deadline - now);
        }
        return true;
    }

private:
    friend class WithholdingVideoDecoder;

    mutable Mutex m_mutex;
    ConditionVariable m_released_changed { m_mutex };
    ConditionVariable m_withheld_changed { m_mutex };
    ConditionVariable m_caller_wait_changed { m_mutex };
    Vector<Output> m_withheld;
    Vector<Output> m_released;
    Function<void()> m_output_arrived_callback;
    bool m_caller_is_waiting_for_output { false };
    size_t m_handed_out_count { 0 };
    Optional<size_t> m_output_index_that_loses_state;
    bool m_loses_state_at_end_of_stream { false };
    bool m_state_was_lost { false };
};

// Produces a blank frame for every coded frame it is given, and withholds each one until the test releases it.
class WithholdingVideoDecoder final : public Media::VideoDecoder {
public:
    AK_ALLOC_WITH_KMALLOC;

    static constexpr Gfx::Size<u32> FRAME_SIZE { 16, 16 };
    static constexpr size_t MAXIMUM_WITHHELD_OUTPUTS = 4;

    // Decoders created while this is set withhold their outputs there; the installed registration is skipped otherwise.
    static void set_withheld_outputs_for_new_decoders(RefPtr<WithheldOutputs> withheld_outputs)
    {
        MutexLocker locker { *s_withheld_outputs_mutex };
        *s_withheld_outputs_for_new_decoders = move(withheld_outputs);
    }

    static ReadonlySpan<Media::VideoDecoderRegistration> registrations()
    {
        static constexpr Array registrations {
            Media::VideoDecoderRegistration { capabilities, create },
        };
        return registrations.span();
    }

    virtual ~WithholdingVideoDecoder() override
    {
        MutexLocker locker { m_withheld_outputs->m_mutex };
        m_withheld_outputs->m_output_arrived_callback = nullptr;
    }

    virtual void set_storage_freed_callback(Function<void()> callback) override { m_frame_pool->set_slot_freed_callback(move(callback)); }
    virtual void set_output_arrived_callback(Function<void()> callback) override
    {
        MutexLocker locker { m_withheld_outputs->m_mutex };
        m_withheld_outputs->m_output_arrived_callback = move(callback);
    }

    virtual Media::DecoderErrorOr<void> receive_coded_data(Media::CodedFrame const& coded_frame, Media::DecodeIntent intent) override
    {
        if (intent == Media::DecodeIntent::Reference)
            return {};
        MutexLocker locker { m_withheld_outputs->m_mutex };
        // A device decoding as many frames as it can at once takes the next one only when one of them finishes.
        while (m_withheld_outputs->m_withheld.size() >= MAXIMUM_WITHHELD_OUTPUTS) {
            m_withheld_outputs->m_caller_is_waiting_for_output = true;
            m_withheld_outputs->m_caller_wait_changed.broadcast();
            m_withheld_outputs->m_released_changed.wait();
            m_withheld_outputs->m_caller_is_waiting_for_output = false;
        }
        m_withheld_outputs->m_withheld.append({ coded_frame.presentation_timestamp(), coded_frame.duration() });
        m_withheld_outputs->m_withheld_changed.broadcast();
        return {};
    }

    virtual void signal_end_of_stream() override { m_reached_end_of_stream = true; }

    virtual Media::DecoderErrorOr<NonnullRefPtr<Media::VideoFrame>> take_next_output(Media::CodingIndependentCodePoints const& container_cicp, Optional<AK::Duration>) override
    {
        MutexLocker locker { m_withheld_outputs->m_mutex };
        // Draining waits for the withheld outputs, as a decoder waits for its device to finish them, and so does
        // holding as many as a device would decode at once.
        auto must_wait_for_a_release = [&] {
            if (!m_withheld_outputs->m_released.is_empty() || m_withheld_outputs->m_withheld.is_empty())
                return false;
            return m_reached_end_of_stream || m_withheld_outputs->m_withheld.size() >= MAXIMUM_WITHHELD_OUTPUTS;
        };
        while (must_wait_for_a_release()) {
            m_withheld_outputs->m_caller_is_waiting_for_output = true;
            m_withheld_outputs->m_caller_wait_changed.broadcast();
            m_withheld_outputs->m_released_changed.wait();
            m_withheld_outputs->m_caller_is_waiting_for_output = false;
        }
        if (m_withheld_outputs->m_released.is_empty()) {
            if (m_reached_end_of_stream) {
                if (exchange(m_withheld_outputs->m_loses_state_at_end_of_stream, false)) {
                    m_withheld_outputs->m_state_was_lost = true;
                    return Media::DecoderError::with_description(Media::DecoderErrorCategory::NeedsResubmission, "The decoder lost its state"sv);
                }
                return Media::DecoderError::with_description(Media::DecoderErrorCategory::EndOfStream, "Every withheld output has been taken"sv);
            }
            return Media::DecoderError::with_description(Media::DecoderErrorCategory::NeedsMoreInput, "No withheld output has been released"sv);
        }

        if (m_withheld_outputs->m_output_index_that_loses_state == m_withheld_outputs->m_handed_out_count) {
            m_withheld_outputs->m_output_index_that_loses_state.clear();
            m_withheld_outputs->m_state_was_lost = true;
            return Media::DecoderError::with_description(Media::DecoderErrorCategory::NeedsResubmission, "The decoder lost its state"sv);
        }

        auto byte_count = FRAME_SIZE.width() * FRAME_SIZE.height() * 3 / 2;
        auto acquired_slot = m_frame_pool->try_acquire(byte_count);
        if (!acquired_slot.has_value())
            return Media::DecoderError::with_description(Media::DecoderErrorCategory::TryAgain, "Every video frame slot is held elsewhere"sv);
        auto pool_slot = m_frame_pool->try_adopt_acquired_slot(*acquired_slot);
        if (pool_slot.is_error())
            return Media::DecoderError::with_description(Media::DecoderErrorCategory::Memory, "Failed to adopt a pooled frame slot"sv);

        auto output = m_withheld_outputs->m_released.take_first();
        m_withheld_outputs->m_handed_out_count++;
        return make_ref_counted<Media::VideoFrame>(output.timestamp, output.duration, FRAME_SIZE, 8, Media::Subsampling::yuv420(), container_cicp, pool_slot.release_value());
    }

    virtual void flush() override
    {
        m_reached_end_of_stream = false;
        MutexLocker locker { m_withheld_outputs->m_mutex };
        m_withheld_outputs->m_withheld.clear();
        m_withheld_outputs->m_released.clear();
    }

private:
    static Optional<Media::DecoderCapabilities> capabilities(Media::ParsedCodec const& codec)
    {
        if (Media::track_type_from_codec_id(codec.codec_id()) != Media::TrackType::Video)
            return {};
        MutexLocker locker { *s_withheld_outputs_mutex };
        if (*s_withheld_outputs_for_new_decoders == nullptr)
            return {};
        return Media::DecoderCapabilities { .smooth = true, .power_efficient = false };
    }

    static Media::DecoderErrorOr<NonnullOwnPtr<Media::VideoDecoder>> create(Media::CodecID, ReadonlyBytes)
    {
        RefPtr<WithheldOutputs> withheld_outputs;
        {
            MutexLocker locker { *s_withheld_outputs_mutex };
            withheld_outputs = *s_withheld_outputs_for_new_decoders;
        }
        if (withheld_outputs == nullptr)
            return Media::DecoderError::with_description(Media::DecoderErrorCategory::NotImplemented, "No test is withholding outputs"sv);
        auto frame_pool = Media::VideoFramePool::create();
        if (frame_pool.is_error())
            return Media::DecoderError::with_description(Media::DecoderErrorCategory::Memory, "Failed to create a frame pool"sv);
        return NonnullOwnPtr<Media::VideoDecoder> { adopt_own(*new WithholdingVideoDecoder(withheld_outputs.release_nonnull(), frame_pool.release_value())) };
    }

    WithholdingVideoDecoder(NonnullRefPtr<WithheldOutputs> withheld_outputs, NonnullRefPtr<Media::VideoFramePool> frame_pool)
        : m_withheld_outputs(move(withheld_outputs))
        , m_frame_pool(move(frame_pool))
    {
    }

    static inline NeverDestroyed<Mutex> s_withheld_outputs_mutex;
    static inline NeverDestroyed<RefPtr<WithheldOutputs>> s_withheld_outputs_for_new_decoders;

    NonnullRefPtr<WithheldOutputs> m_withheld_outputs;
    NonnullRefPtr<Media::VideoFramePool> m_frame_pool;
    bool m_reached_end_of_stream { false };
};
