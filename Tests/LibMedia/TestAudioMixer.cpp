/*
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibAudio/ChannelMap.h>
#include <LibAudio/SampleSpecification.h>
#include <LibMedia/Processors/AudioMixer.h>
#include <LibMedia/Producers/AudioProducer.h>
#include <LibTest/TestCase.h>

namespace {

// An input that always has one block of silence to mix.
class ScriptedAudioProducer final : public Media::AudioProducer {
public:
    void start() override { }
    void consume() override { m_block.clear(); }
    void set_wake_handler(Media::PipelineWakeHandler) override { }
    void seek(AK::Duration) override { }

    ErrorOr<void> set_output_sample_specification(Audio::SampleSpecification specification) override
    {
        m_specification = specification;
        return {};
    }

    Media::AudioProducerOutput peek() override
    {
        if (m_block.is_empty()) {
            m_block.initialize(m_specification, static_cast<i64>(0), 128);
            for (size_t channel = 0; channel < m_specification.channel_count(); ++channel)
                m_block.channel_data(channel).fill(0.0f);
        }
        return { &m_block, Media::PipelineStatus::HaveData };
    }

private:
    Audio::SampleSpecification m_specification;
    Media::AudioBlock m_block;
};

// An input with no data to mix, which reports whatever status and checkpoint the test gives it.
class WaitingAudioProducer final : public Media::AudioProducer {
public:
    void start() override { }
    void consume() override { }
    void set_wake_handler(Media::PipelineWakeHandler) override { }
    void seek(AK::Duration) override { }
    ErrorOr<void> set_output_sample_specification(Audio::SampleSpecification) override { return {}; }

    Media::AudioProducerOutput peek() override { return { nullptr, m_status, m_checkpoint }; }

    void set_status(Media::PipelineStatus status) { m_status = status; }
    void set_state(Media::PipelineStatus status, Media::PipelineCheckpoint checkpoint)
    {
        m_status = status;
        m_checkpoint = checkpoint;
    }

private:
    Media::PipelineStatus m_status { Media::PipelineStatus::Blocked };
    Media::PipelineCheckpoint m_checkpoint { 0 };
};

}

// A seek to a timestamp whose frame index saturates must not overflow the mixer's frame window arithmetic.
TEST_CASE(seek_to_unrepresentable_timestamp_ends_the_stream)
{
    auto mixer = MUST(Media::AudioMixer::try_create());
    MUST(mixer->connect_input(adopt_ref(*new ScriptedAudioProducer())));
    MUST(mixer->set_output_sample_specification(Audio::SampleSpecification(48'000, Audio::ChannelMap::stereo())));

    mixer->seek(AK::Duration::max());
    mixer->start();

    auto output = mixer->peek();
    EXPECT_EQ(output.status, Media::PipelineStatus::EndOfStream);
    EXPECT_EQ(output.block, nullptr);
}

// A mix stops at the first input that has nothing to mix, but the mixer still reports the status of every input, so that
// it is blocked while any of them is.
TEST_CASE(the_status_of_every_input_is_combined)
{
    auto mixer = MUST(Media::AudioMixer::try_create());
    auto first_input = adopt_ref(*new WaitingAudioProducer());
    auto second_input = adopt_ref(*new WaitingAudioProducer());
    MUST(mixer->connect_input(first_input));
    MUST(mixer->connect_input(second_input));
    MUST(mixer->set_output_sample_specification(Audio::SampleSpecification(48'000, Audio::ChannelMap::stereo())));
    mixer->seek(AK::Duration::zero());
    mixer->start();

    EXPECT_EQ(mixer->peek().status, Media::PipelineStatus::Blocked);

    first_input->set_status(Media::PipelineStatus::Pending);
    EXPECT_EQ(mixer->peek().status, Media::PipelineStatus::Blocked);

    first_input->set_status(Media::PipelineStatus::Blocked);
    second_input->set_status(Media::PipelineStatus::Pending);
    EXPECT_EQ(mixer->peek().status, Media::PipelineStatus::Blocked);

    first_input->set_status(Media::PipelineStatus::Pending);
    EXPECT_EQ(mixer->peek().status, Media::PipelineStatus::Pending);
}

// A mix only reflects a change once every input has been observed after it, so the mixer passes on the oldest checkpoint
// among its inputs, each taken with that input's status.
TEST_CASE(the_oldest_checkpoint_among_the_inputs_is_passed_on)
{
    auto mixer = MUST(Media::AudioMixer::try_create());
    auto first_input = adopt_ref(*new WaitingAudioProducer());
    auto second_input = adopt_ref(*new WaitingAudioProducer());
    MUST(mixer->connect_input(first_input));
    MUST(mixer->connect_input(second_input));
    MUST(mixer->set_output_sample_specification(Audio::SampleSpecification(48'000, Audio::ChannelMap::stereo())));
    mixer->seek(AK::Duration::zero());
    mixer->start();

    auto output = mixer->peek();
    EXPECT_EQ(output.status, Media::PipelineStatus::Blocked);
    EXPECT_EQ(output.checkpoint, 0u);

    // The first input has been given checkpoint 1 and continues, but the second has not been given it yet.
    first_input->set_state(Media::PipelineStatus::Pending, 1);
    output = mixer->peek();
    EXPECT_EQ(output.status, Media::PipelineStatus::Blocked);
    EXPECT_EQ(output.checkpoint, 0u);

    second_input->set_state(Media::PipelineStatus::Blocked, 1);
    output = mixer->peek();
    EXPECT_EQ(output.status, Media::PipelineStatus::Blocked);
    EXPECT_EQ(output.checkpoint, 1u);

    first_input->set_state(Media::PipelineStatus::Blocked, 2);
    second_input->set_state(Media::PipelineStatus::Pending, 2);
    output = mixer->peek();
    EXPECT_EQ(output.status, Media::PipelineStatus::Blocked);
    EXPECT_EQ(output.checkpoint, 2u);

    first_input->set_state(Media::PipelineStatus::Pending, 3);
    second_input->set_state(Media::PipelineStatus::Pending, 3);
    output = mixer->peek();
    EXPECT_EQ(output.status, Media::PipelineStatus::Pending);
    EXPECT_EQ(output.checkpoint, 3u);
}
