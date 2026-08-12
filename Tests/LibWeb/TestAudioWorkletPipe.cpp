/*
 * Copyright (c) 2026, Ali Mohammad Pur <ali@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/BuiltinWrappers.h>
#include <AK/Math.h>
#include <AK/Optional.h>
#include <AK/Random.h>
#include <AK/Vector.h>
#include <LibCore/EventLoop.h>
#include <LibCore/System.h>
#include <LibCore/Timer.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>
#include <LibWeb/WebAudio/Rendering/AudioWorkletPipe.h>

using Web::WebAudio::Rendering::AudioWorkletPipe;

static constexpr size_t QUANTUM_SIZE = 128;
static constexpr size_t RING_CAPACITY = 64;
static constexpr size_t PRIME_LEVEL = 48;
static constexpr size_t BURST_QUANTA = 38;

static AudioWorkletPipe::Config test_config()
{
    return {
        .quantum_size = QUANTUM_SIZE,
        .sample_rate = 48000.0f,
        .input_count = 1,
        .output_count = 1,
        .input_channel_capacity = { 2 },
        .output_channel_capacity = { 2 },
        .param_count = 2,
        .param_is_a_rate = { true, false },
        .ring_capacity = RING_CAPACITY,
        .prime_level = PRIME_LEVEL,
    };
}

TEST_CASE(ring_sizing_matches_renderer_defaults)
{
    auto sizing = AudioWorkletPipe::Config::ring_sizing_for_device_latency(100, 48000.0f, 128);
    EXPECT_EQ(sizing.ring_capacity, 64u);
    EXPECT_EQ(sizing.prime_level, 48u);

    for (u32 latency_ms : { 10u, 20u, 50u, 100u, 200u, 500u }) {
        auto s = AudioWorkletPipe::Config::ring_sizing_for_device_latency(latency_ms, 48000.0f, 128);
        auto burst = static_cast<size_t>(AK::ceil(latency_ms / 1000.0 * 48000.0 / 128));
        EXPECT(s.prime_level >= burst);
        EXPECT(s.ring_capacity > s.prime_level);
        EXPECT_EQ(popcount(s.ring_capacity), 1);
    }
}

TEST_CASE(pops_on_empty_rings_fail)
{
    Core::EventLoop event_loop;
    auto pipe = AudioWorkletPipe::create(test_config(), event_loop);

    EXPECT(!pipe->try_pop_input([](auto const&) { VERIFY_NOT_REACHED(); }));
    EXPECT(!pipe->try_pop_output([](auto const&) { VERIFY_NOT_REACHED(); }));

    // Only the audio thread's output pops count as underruns; an empty input ring just means the pump caught up.
    EXPECT_EQ(pipe->underrun_count(), 1u);
    EXPECT_EQ(pipe->dropped_input_count(), 0u);
    EXPECT_EQ(pipe->output_occupancy(), 0u);
}

TEST_CASE(input_ring_roundtrip_fill_to_capacity_and_overrun)
{
    Core::EventLoop event_loop;
    auto pipe = AudioWorkletPipe::create(test_config(), event_loop);

    for (size_t i = 0; i < RING_CAPACITY; ++i) {
        bool pushed = pipe->try_push_input([&](AudioWorkletPipe::InputSlotWriter& slot) {
            slot.start_frame() = i * QUANTUM_SIZE;
            if (i == 3)
                slot.set_had_gap(true);
            slot.actual_channel_count(0) = static_cast<u32>(i % 3);
            for (u32 channel = 0; channel < i % 3; ++channel) {
                auto samples = slot.input_channel(0, channel);
                EXPECT_EQ(samples.size(), QUANTUM_SIZE);
                for (size_t frame = 0; frame < samples.size(); ++frame)
                    samples[frame] = static_cast<float>(i * 10 + channel) + static_cast<float>(frame) * 0.25f;
            }
            EXPECT_EQ(slot.param_block(0).size(), QUANTUM_SIZE);
            EXPECT_EQ(slot.param_block(1).size(), 1u);
            for (size_t frame = 0; frame < QUANTUM_SIZE; ++frame)
                slot.param_block(0)[frame] = static_cast<float>(i) + static_cast<float>(frame) * 0.5f;
            slot.param_block(1)[0] = static_cast<float>(i);
        });
        EXPECT(pushed);
    }

    EXPECT(!pipe->try_push_input([](auto&) { VERIFY_NOT_REACHED(); }));
    EXPECT_EQ(pipe->dropped_input_count(), 1u);

    for (size_t i = 0; i < RING_CAPACITY; ++i) {
        bool popped = pipe->try_pop_input([&](AudioWorkletPipe::InputSlotReader const& slot) {
            EXPECT_EQ(slot.start_frame(), i * QUANTUM_SIZE);
            EXPECT_EQ(slot.had_gap(), i == 3);
            EXPECT_EQ(slot.actual_channel_count(0), i % 3);
            for (u32 channel = 0; channel < i % 3; ++channel) {
                auto samples = slot.input_channel(0, channel);
                EXPECT_EQ(samples[0], static_cast<float>(i * 10 + channel));
                EXPECT_EQ(samples[QUANTUM_SIZE - 1], static_cast<float>(i * 10 + channel) + (QUANTUM_SIZE - 1) * 0.25f);
            }
            EXPECT_EQ(slot.param_block(0)[0], static_cast<float>(i));
            EXPECT_EQ(slot.param_block(0)[QUANTUM_SIZE - 1], static_cast<float>(i) + (QUANTUM_SIZE - 1) * 0.5f);
            EXPECT_EQ(slot.param_block(1)[0], static_cast<float>(i));
        });
        EXPECT(popped);
    }
    EXPECT(!pipe->try_pop_input([](auto const&) { VERIFY_NOT_REACHED(); }));

    // Wrap around into the same storage: flags are cleared on push, so slot 3's gap flag must not leak through.
    for (size_t i = 0; i < RING_CAPACITY; ++i) {
        EXPECT(pipe->try_push_input([&](AudioWorkletPipe::InputSlotWriter& slot) {
            slot.start_frame() = (RING_CAPACITY + i) * QUANTUM_SIZE;
            slot.actual_channel_count(0) = 0;
        }));
    }
    for (size_t i = 0; i < RING_CAPACITY; ++i) {
        EXPECT(pipe->try_pop_input([&](AudioWorkletPipe::InputSlotReader const& slot) {
            EXPECT_EQ(slot.start_frame(), (RING_CAPACITY + i) * QUANTUM_SIZE);
            EXPECT(!slot.had_gap());
        }));
    }

    EXPECT_EQ(pipe->dropped_input_count(), 1u);
    EXPECT_EQ(pipe->underrun_count(), 0u);
}

TEST_CASE(output_ring_roundtrip)
{
    Core::EventLoop event_loop;
    auto pipe = AudioWorkletPipe::create(test_config(), event_loop);

    for (size_t i = 0; i < 5; ++i) {
        EXPECT(pipe->try_push_output([&](AudioWorkletPipe::OutputSlotWriter& slot) {
            slot.start_frame() = i * QUANTUM_SIZE;
            slot.set_processor_active(i % 2 == 0);
            slot.actual_channel_count(0) = 2;
            for (u32 channel = 0; channel < 2; ++channel)
                slot.output_channel(0, channel).fill(static_cast<float>(i * 10 + channel));
        }));
    }
    EXPECT_EQ(pipe->output_occupancy(), 5u);

    for (size_t i = 0; i < 5; ++i) {
        EXPECT(pipe->try_pop_output([&](AudioWorkletPipe::OutputSlotReader const& slot) {
            EXPECT_EQ(slot.start_frame(), i * QUANTUM_SIZE);
            EXPECT_EQ(slot.processor_active(), i % 2 == 0);
            EXPECT_EQ(slot.actual_channel_count(0), 2u);
            for (u32 channel = 0; channel < 2; ++channel) {
                EXPECT_EQ(slot.output_channel(0, channel)[0], static_cast<float>(i * 10 + channel));
                EXPECT_EQ(slot.output_channel(0, channel)[QUANTUM_SIZE - 1], static_cast<float>(i * 10 + channel));
            }
        }));
    }
    EXPECT_EQ(pipe->underrun_count(), 0u);
}

TEST_CASE(priming_and_discard_down_to)
{
    Core::EventLoop event_loop;
    auto pipe = AudioWorkletPipe::create(test_config(), event_loop);

    pipe->prime_outputs_with_silence(PRIME_LEVEL);
    EXPECT_EQ(pipe->output_occupancy(), PRIME_LEVEL);

    bool popped = pipe->try_pop_output([&](AudioWorkletPipe::OutputSlotReader const& slot) {
        EXPECT_EQ(slot.start_frame(), 0u);
        EXPECT(slot.processor_active());
        EXPECT_EQ(slot.actual_channel_count(0), 2u);
        for (u32 channel = 0; channel < 2; ++channel) {
            for (auto sample : slot.output_channel(0, channel))
                EXPECT_EQ(sample, 0.0f);
        }
    });
    EXPECT(popped);
    EXPECT_EQ(pipe->output_occupancy(), PRIME_LEVEL - 1);

    pipe->discard_outputs_down_to(10);
    EXPECT_EQ(pipe->output_occupancy(), 10u);

    pipe->discard_outputs_down_to(20);
    EXPECT_EQ(pipe->output_occupancy(), 10u);

    pipe->discard_outputs_down_to(0);
    EXPECT_EQ(pipe->output_occupancy(), 0u);
    EXPECT_EQ(pipe->underrun_count(), 0u);
}

TEST_CASE(state_transitions)
{
    Core::EventLoop event_loop;
    auto pipe = AudioWorkletPipe::create(test_config(), event_loop);

    EXPECT(pipe->state() == AudioWorkletPipe::State::Running);
    pipe->set_state(AudioWorkletPipe::State::Failed);
    EXPECT(pipe->state() == AudioWorkletPipe::State::Failed);
    pipe->request_shutdown();
    EXPECT(pipe->state() == AudioWorkletPipe::State::ShutDown);
}

TEST_CASE(wakeup_coalescing)
{
    Core::EventLoop event_loop;
    auto pipe = AudioWorkletPipe::create(test_config(), event_loop);

    size_t pump_runs = 0;
    pipe->set_pump_callback([&] { ++pump_runs; });

    pipe->request_wakeup();
    pipe->request_wakeup();
    pipe->request_wakeup();
    event_loop.spin_until([&] { return pump_runs > 0; });
    // Give any (incorrectly) duplicated wakeups a chance to run.
    event_loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(pump_runs, 1u);

    pipe->clear_wakeup_flag();
    pipe->request_wakeup();
    event_loop.spin_until([&] { return pump_runs > 1; });
    EXPECT_EQ(pump_runs, 2u);

    pipe->clear_wakeup_flag();
    pipe->request_shutdown();
    pipe->request_wakeup();
    event_loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(pump_runs, 2u);
}

TEST_CASE(two_thread_stress)
{
    Core::EventLoop event_loop;
    auto config = test_config();
    auto pipe = AudioWorkletPipe::create(config, event_loop);
    pipe->prime_outputs_with_silence(PRIME_LEVEL);

    static constexpr size_t BURST_COUNT = 20;
    static constexpr u64 FIRST_STAMP = QUANTUM_SIZE; // Primed slots are stamped 0; real quanta start one quantum in.

    u64 quanta_processed = 0;
    pipe->set_pump_callback([&] {
        pipe->clear_wakeup_flag();
        MUST(Core::System::sleep_ms(get_random_uniform(11)));
        while (pipe->try_pop_input([&](AudioWorkletPipe::InputSlotReader const& input) {
            bool pushed = pipe->try_push_output([&](AudioWorkletPipe::OutputSlotWriter& output) {
                output.start_frame() = input.start_frame();
                output.set_processor_active(true);
                output.actual_channel_count(0) = input.actual_channel_count(0);
                auto gain = input.param_block(1)[0];
                for (u32 channel = 0; channel < input.actual_channel_count(0); ++channel) {
                    auto source = input.input_channel(0, channel);
                    auto destination = output.output_channel(0, channel);
                    for (size_t frame = 0; frame < source.size(); ++frame)
                        destination[frame] = source[frame] * gain;
                }
            });
            EXPECT(pushed);
            ++quanta_processed;
        })) { }
    });

    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> producer_done { false };
    IGNORE_USE_IN_ESCAPING_LAMBDA Vector<u64> popped_stamps;
    popped_stamps.ensure_capacity(BURST_COUNT * BURST_QUANTA + PRIME_LEVEL);
    IGNORE_USE_IN_ESCAPING_LAMBDA u64 sample_mismatches = 0;
    IGNORE_USE_IN_ESCAPING_LAMBDA u64 failed_output_pops = 0;
    IGNORE_USE_IN_ESCAPING_LAMBDA u64 successful_input_pushes = 0;

    auto producer = Threading::Thread::construct("AudioWorkletBurst"sv, [&, producer_pipe = NonnullRefPtr(*pipe)]() -> intptr_t {
        u64 next_frame = FIRST_STAMP;
        for (size_t burst = 0; burst < BURST_COUNT; ++burst) {
            for (size_t quantum = 0; quantum < BURST_QUANTA; ++quantum) {
                bool popped = producer_pipe->try_pop_output([&](AudioWorkletPipe::OutputSlotReader const& output) {
                    auto stamp = output.start_frame();
                    popped_stamps.unchecked_append(stamp);
                    // Primed slots are silent; processed ones echo the value the producer filled in below.
                    for (u32 channel = 0; channel < output.actual_channel_count(0); ++channel) {
                        auto expected = stamp == 0 ? 0.0f : static_cast<float>(stamp / QUANTUM_SIZE + channel);
                        if (output.output_channel(0, channel)[0] != expected)
                            ++sample_mismatches;
                    }
                });
                if (!popped)
                    ++failed_output_pops;
                bool pushed = producer_pipe->try_push_input([&](AudioWorkletPipe::InputSlotWriter& input) {
                    input.start_frame() = next_frame;
                    input.actual_channel_count(0) = 2;
                    for (u32 channel = 0; channel < 2; ++channel)
                        input.input_channel(0, channel).fill(static_cast<float>(next_frame / QUANTUM_SIZE + channel));
                    input.param_block(0).fill(0.5f);
                    input.param_block(1)[0] = 1.0f;
                });
                if (pushed)
                    ++successful_input_pushes;
                next_frame += QUANTUM_SIZE;
            }
            producer_pipe->request_wakeup();
            MUST(Core::System::sleep_ms(100));
        }
        producer_done.store(true);
        return 0;
    });
    producer->start();

    IGNORE_USE_IN_ESCAPING_LAMBDA Core::EventLoop& loop = event_loop;
    auto quit_timer = Core::Timer::create_repeating(25, [&] {
        if (producer_done.load())
            loop.quit(0);
    });
    quit_timer->start();
    event_loop.exec();
    MUST(producer->join());

    u64 pending_input = 0;
    while (pipe->try_pop_input([&](AudioWorkletPipe::InputSlotReader const&) {
        ++pending_input;
    })) { }

    EXPECT_EQ(sample_mismatches, 0u);
    EXPECT_EQ(failed_output_pops + popped_stamps.size(), BURST_COUNT * BURST_QUANTA);
    EXPECT_EQ(successful_input_pushes + pipe->dropped_input_count(), BURST_COUNT * BURST_QUANTA);
    EXPECT_EQ(quanta_processed + pending_input, successful_input_pushes);

    // The FIFO order guarantees all primed (stamp 0) slots come out before any processed quantum, and processed
    // stamps are observed in increasing quantum order.
    size_t primed_popped = 0;
    while (primed_popped < popped_stamps.size() && popped_stamps[primed_popped] == 0)
        ++primed_popped;
    EXPECT(primed_popped <= PRIME_LEVEL);
    Optional<u64> previous_stamp;
    for (size_t i = primed_popped; i < popped_stamps.size(); ++i) {
        auto stamp = popped_stamps[i];
        EXPECT(stamp >= FIRST_STAMP);
        EXPECT_EQ((stamp - FIRST_STAMP) % QUANTUM_SIZE, 0u);
        if (previous_stamp.has_value())
            EXPECT(stamp > previous_stamp.value());
        previous_stamp = stamp;
    }
}

TEST_CASE(teardown_when_producer_drops_its_reference_mid_run)
{
    Core::EventLoop event_loop;
    auto config = test_config();
    auto pipe = AudioWorkletPipe::create(config, event_loop);
    pipe->prime_outputs_with_silence(PRIME_LEVEL);

    u64 quanta_processed = 0;
    pipe->set_pump_callback([&] {
        pipe->clear_wakeup_flag();
        if (pipe->state() != AudioWorkletPipe::State::Running)
            return;
        while (pipe->try_pop_input([&](AudioWorkletPipe::InputSlotReader const& input) {
            (void)pipe->try_push_output([&](AudioWorkletPipe::OutputSlotWriter& output) {
                output.start_frame() = input.start_frame();
                output.set_processor_active(true);
                output.actual_channel_count(0) = 0;
            });
            ++quanta_processed;
        })) { }
    });

    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> producer_done { false };
    auto producer = Threading::Thread::construct("AudioWorkletTeardown"sv, [&, producer_pipe = RefPtr<AudioWorkletPipe>(pipe)]() mutable -> intptr_t {
        u64 next_frame = QUANTUM_SIZE;
        for (size_t burst = 0; burst < 3; ++burst) {
            for (size_t quantum = 0; quantum < BURST_QUANTA; ++quantum) {
                producer_pipe->try_pop_output([](auto const&) { });
                (void)producer_pipe->try_push_input([&](AudioWorkletPipe::InputSlotWriter& input) {
                    input.start_frame() = next_frame;
                    input.actual_channel_count(0) = 0;
                });
                next_frame += QUANTUM_SIZE;
            }
            producer_pipe->request_wakeup();
            MUST(Core::System::sleep_ms(30));
        }
        producer_pipe->request_shutdown();
        producer_pipe = nullptr;
        producer_done.store(true);
        return 0;
    });
    producer->start();

    IGNORE_USE_IN_ESCAPING_LAMBDA Core::EventLoop& loop = event_loop;
    auto quit_timer = Core::Timer::create_repeating(10, [&] {
        if (producer_done.load())
            loop.quit(0);
    });
    quit_timer->start();
    event_loop.exec();
    MUST(producer->join());

    EXPECT(pipe->state() == AudioWorkletPipe::State::ShutDown);
    EXPECT_EQ(pipe->dropped_input_count(), 0u);
    EXPECT(quanta_processed > 0);
}
