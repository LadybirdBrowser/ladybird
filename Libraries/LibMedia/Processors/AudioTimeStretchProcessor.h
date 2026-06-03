/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <LibMedia/Audio/SampleSpecification.h>
#include <LibMedia/Audio/TimeStretcher.h>
#include <LibMedia/AudioBlock.h>
#include <LibMedia/Export.h>
#include <LibMedia/PipelineStatus.h>
#include <LibMedia/Processors/AudioProcessor.h>
#include <LibMedia/Producers/AudioProducer.h>
#include <LibSync/Mutex.h>

namespace Media {

class MEDIA_API AudioTimeStretchProcessor final : public AudioProcessor {
public:
    static ErrorOr<NonnullRefPtr<AudioTimeStretchProcessor>> try_create();
    AudioTimeStretchProcessor();
    virtual ~AudioTimeStretchProcessor() override;

    virtual ErrorOr<void> connect_input(NonnullRefPtr<AudioProducer> const&) override;
    virtual void disconnect_input(NonnullRefPtr<AudioProducer> const&) override;

    virtual void seek(AK::Duration) override;
    virtual ErrorOr<void> set_output_sample_specification(Audio::SampleSpecification) override;
    virtual void start() override;

    virtual PipelineStatus status() const override;
    virtual void pull(AudioBlock&) override;
    virtual void set_wake_handler(PipelineWakeHandler) override;
    virtual void set_playback_rate(float) override;

private:
    void ensure_stretcher_while_locked() const;
    void maybe_recover_from_stale_upstream_eos_while_locked() const;
    PipelineStatus produce_block_while_locked(AudioBlock&) const;
    void dispatch_wake();

    mutable Sync::Mutex m_mutex;
    Audio::SampleSpecification m_sample_specification;
    RefPtr<AudioProducer> m_input;

    float m_playback_rate { 1.0f };
    mutable OwnPtr<Audio::TimeStretcher> m_stretcher;
    bool m_started { false };

    mutable i64 m_next_output_frame { 0 };
    mutable AK::Duration m_next_emit_media_time;
    mutable bool m_stretcher_reached_eos { false };

    mutable AudioBlock m_input_block;
    mutable AudioBlock m_pending_block;
    mutable bool m_downstream_needs_wake { true };
    bool m_moved_position_pending { false };

    PipelineWakeHandler m_wake_handler;
};

}
