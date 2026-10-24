/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/ConditionVariable.h>
#include <AK/Mutex.h>
#include <AK/NeverDestroyed.h>
#include <AK/RefPtr.h>
#include <LibMedia/DecoderRegistrations.h>
#include <LibMediaCodecs/AudioDecoder.h>
#include <LibMediaCodecs/AudioDecoderRegistry.h>

// Tells a StateLosingAudioDecoder when to lose its state, and holds the decoding that follows until the test resumes it.
class AudioDecoderStateLoss final : public AtomicRefCounted<AudioDecoderStateLoss> {
public:
    explicit AudioDecoderStateLoss(size_t block_index_that_loses_state)
        : m_block_index_that_loses_state(block_index_that_loses_state)
    {
    }

    bool wait_until_decoding_is_held_after_the_loss(AK::Duration timeout)
    {
        auto deadline = MonotonicTime::now() + timeout;
        MutexLocker locker { m_mutex };
        while (!m_decoding_is_held) {
            auto now = MonotonicTime::now();
            if (now >= deadline)
                return false;
            m_state_changed.wait_for(deadline - now);
        }
        return true;
    }

    void resume_decoding()
    {
        MutexLocker locker { m_mutex };
        m_holds_decoding = false;
        m_state_changed.broadcast();
    }

private:
    friend class StateLosingAudioDecoder;

    Mutex m_mutex;
    ConditionVariable m_state_changed { m_mutex };
    size_t const m_block_index_that_loses_state;
    size_t m_written_block_count { 0 };
    bool m_state_was_lost { false };
    bool m_holds_decoding { true };
    bool m_decoding_is_held { false };
};

// Decodes with the first installed decoder that accepts the codec, but reports that it lost its state once, instead of
// writing the block at the chosen index.
class StateLosingAudioDecoder final : public Media::AudioDecoder {
public:
    AK_ALLOC_WITH_KMALLOC;

    // Decoders created while this is set lose their state there; the installed registration is skipped otherwise.
    static void set_state_loss_for_new_decoders(RefPtr<AudioDecoderStateLoss> state_loss)
    {
        MutexLocker locker { *s_state_loss_mutex };
        *s_state_loss_for_new_decoders = move(state_loss);
    }

    static ReadonlySpan<Media::AudioDecoderRegistration> registrations()
    {
        static constexpr Array registrations {
            Media::AudioDecoderRegistration { capabilities, create },
        };
        return registrations.span();
    }

    virtual Media::DecoderErrorOr<void> receive_coded_data(Media::CodedFrame const& coded_frame) override
    {
        {
            MutexLocker locker { m_state_loss->m_mutex };
            while (m_state_loss->m_state_was_lost && m_state_loss->m_holds_decoding) {
                m_state_loss->m_decoding_is_held = true;
                m_state_loss->m_state_changed.broadcast();
                m_state_loss->m_state_changed.wait();
            }
        }
        return m_inner->receive_coded_data(coded_frame);
    }

    virtual void signal_end_of_stream() override { m_inner->signal_end_of_stream(); }

    virtual Media::DecoderErrorOr<void> write_next_block(Media::AudioBlock& block) override
    {
        {
            MutexLocker locker { m_state_loss->m_mutex };
            if (!m_state_loss->m_state_was_lost && m_state_loss->m_written_block_count == m_state_loss->m_block_index_that_loses_state) {
                m_state_loss->m_state_was_lost = true;
                return Media::DecoderError::with_description(Media::DecoderErrorCategory::NeedsResubmission, "The decoder lost its state"sv);
            }
        }
        TRY(m_inner->write_next_block(block));
        MutexLocker locker { m_state_loss->m_mutex };
        m_state_loss->m_written_block_count++;
        return {};
    }

    virtual void flush() override { m_inner->flush(); }

private:
    static Optional<Media::DecoderCapabilities> capabilities(Media::ParsedCodec const& codec)
    {
        {
            MutexLocker locker { *s_state_loss_mutex };
            if (*s_state_loss_for_new_decoders == nullptr)
                return {};
        }
        for (auto const& registration : Media::audio_decoders()) {
            if (auto capabilities = registration.capabilities(codec); capabilities.has_value())
                return capabilities;
        }
        return {};
    }

    static Media::DecoderErrorOr<NonnullOwnPtr<Media::AudioDecoder>> create(Media::CodecID codec_id, Audio::SampleSpecification const& sample_specification, ReadonlyBytes codec_initialization_data)
    {
        RefPtr<AudioDecoderStateLoss> state_loss;
        {
            MutexLocker locker { *s_state_loss_mutex };
            state_loss = *s_state_loss_for_new_decoders;
        }
        if (state_loss == nullptr)
            return Media::DecoderError::with_description(Media::DecoderErrorCategory::NotImplemented, "No test is losing decoder state"sv);
        for (auto const& registration : Media::audio_decoders()) {
            auto inner = registration.create(codec_id, sample_specification, codec_initialization_data);
            if (!inner.is_error())
                return NonnullOwnPtr<Media::AudioDecoder> { adopt_own(*new StateLosingAudioDecoder(state_loss.release_nonnull(), inner.release_value())) };
        }
        return Media::DecoderError::with_description(Media::DecoderErrorCategory::NotImplemented, "No installed decoder accepts the codec"sv);
    }

    StateLosingAudioDecoder(NonnullRefPtr<AudioDecoderStateLoss> state_loss, NonnullOwnPtr<Media::AudioDecoder> inner)
        : m_state_loss(move(state_loss))
        , m_inner(move(inner))
    {
    }

    static inline NeverDestroyed<Mutex> s_state_loss_mutex;
    static inline NeverDestroyed<RefPtr<AudioDecoderStateLoss>> s_state_loss_for_new_decoders;

    NonnullRefPtr<AudioDecoderStateLoss> m_state_loss;
    NonnullOwnPtr<Media::AudioDecoder> m_inner;
};
