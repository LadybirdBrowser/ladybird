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

    virtual ErrorOr<void> set_output_sample_specification(Audio::SampleSpecification) override;
    Audio::SampleSpecification sample_specification() const;

    virtual void start() override;

    virtual PipelineStatus pull(AudioBlock& into) override;

    virtual void set_state_changed_handler(PipelineStateChangeHandler) override;

private:
    struct InputMixingData {
        AudioBlock current_block;
        i64 next_frame { 0 };
        PipelineStatus last_status { PipelineStatus::Pending };
    };

    void dispatch_state(PipelineStatus);
    AK::Duration mix_head_timestamp() const;

    void disconnect_input_while_locked(NonnullRefPtr<AudioProducer> const&);

    mutable Sync::Mutex m_mutex;
    Audio::SampleSpecification m_sample_specification;
    HashMap<NonnullRefPtr<AudioProducer>, InputMixingData> m_inputs;
    i64 m_next_frame_to_write { 0 };
    bool m_started { false };

    PipelineStateChangeHandler m_state_changed_handler;
};

}
