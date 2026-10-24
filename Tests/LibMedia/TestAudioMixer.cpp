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
