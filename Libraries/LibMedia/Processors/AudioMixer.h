/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <LibMedia/Audio/Forward.h>
#include <LibMedia/Audio/SampleSpecification.h>
#include <LibMedia/AudioBlock.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>
#include <LibMedia/PipelineStatus.h>
#include <LibMedia/Processors/AudioProcessor.h>
#include <LibMedia/Producers/AudioProducer.h>
#include <LibSync/Mutex.h>

namespace Media {

class MEDIA_API AudioMixer final : public AudioProcessor {
public:
    static ErrorOr<NonnullRefPtr<AudioMixer>> try_create();
    AudioMixer();
    virtual ~AudioMixer() override;

    virtual ErrorOr<void> connect_input(NonnullRefPtr<AudioProducer> const&) override;
    virtual void disconnect_input(NonnullRefPtr<AudioProducer> const&) override;

    virtual void seek(AK::Duration timestamp) override;

    virtual void set_playback_rate(float) override;

    virtual ErrorOr<void> set_output_sample_specification(Audio::SampleSpecification) override;
    Audio::SampleSpecification sample_specification() const;

    virtual void start() override;

    virtual PipelineStatus status() const override;
    virtual void pull(AudioBlock& into) override;

    virtual void set_wake_handler(PipelineWakeHandler) override;

private:
    struct InputMixingData {
        AudioBlock current_block;
        i64 next_frame { 0 };
        PipelineStatus last_status { PipelineStatus::Pending };
    };

    PipelineStatus combined_input_status() const;
    void dispatch_wake();
    AK::Duration mix_head_timestamp() const;

    void disconnect_input_while_locked(NonnullRefPtr<AudioProducer> const&);

    mutable Sync::Mutex m_mutex;
    Audio::SampleSpecification m_sample_specification;
    mutable HashMap<NonnullRefPtr<AudioProducer>, InputMixingData> m_inputs;
    i64 m_next_frame_to_write { 0 };
    float m_playback_rate { 1.0f };
    bool m_started { false };
    bool m_moved_position_pending { false };

    PipelineWakeHandler m_wake_handler;
    mutable PipelineStatus m_status { PipelineStatus::Pending };
    mutable PipelineStatus m_last_returned_status { PipelineStatus::Pending };
};

}
