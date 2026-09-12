/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <LibCore/EventLoop.h>
#include <LibMedia/Audio/AudioOutputMonitor.h>

namespace Audio {

AudioOutputMonitor::State AudioOutputMonitor::advance_transition_sequence_number(State state)
{
    auto const flags = state & State::FlagMask;
    auto const next_transition_sequence_number = to_underlying(state & ~State::FlagMask) + to_underlying(State::TransitionSequenceIncrement);
    return static_cast<State>(next_transition_sequence_number) | flags;
}

u64 AudioOutputMonitor::transition_sequence_number(State state)
{
    return to_underlying(state) >> STATE_FLAG_BIT_COUNT;
}

NonnullRefPtr<AudioOutputMonitor> AudioOutputMonitor::create(Core::EventLoop& main_thread_event_loop)
{
    return adopt_ref(*new AudioOutputMonitor(main_thread_event_loop));
}

AudioOutputMonitor::AudioOutputMonitor(Core::EventLoop& main_thread_event_loop)
    : m_main_thread_event_loop(main_thread_event_loop)
{
}

void AudioOutputMonitor::set_output_state_change_handler(Function<void(bool)> handler)
{
    m_on_output_state_change = move(handler);
    auto const state = m_state.load();
    m_last_delivered_transition_sequence_number = transition_sequence_number(state);
    if (m_on_output_state_change && has_flag(state, State::IsNonSilent))
        m_on_output_state_change(true);
}

void AudioOutputMonitor::set_enabled(bool enabled)
{
    update_configuration(State::Enabled, enabled);
}

void AudioOutputMonitor::set_muted(bool muted)
{
    update_configuration(State::Muted, muted);
}

void AudioOutputMonitor::update(bool output_is_non_silent)
{
    auto state = m_state.load();
    for (;;) {
        auto const is_non_silent = output_is_non_silent
            && has_flag(state, State::Enabled)
            && !has_flag(state, State::Muted);
        if (has_flag(state, State::IsNonSilent) == is_non_silent)
            return;

        auto updated_state = advance_transition_sequence_number(state);
        if (is_non_silent)
            updated_state |= State::IsNonSilent;
        else
            updated_state &= ~State::IsNonSilent;

        if (!m_state.compare_exchange_strong(state, updated_state))
            continue;

        dispatch_output_state_change(is_non_silent, transition_sequence_number(updated_state));
        return;
    }
}

void AudioOutputMonitor::update_configuration(State flag, bool value)
{
    auto state = m_state.load();
    for (;;) {
        if (has_flag(state, flag) == value)
            return;

        auto updated_state = advance_transition_sequence_number(state);
        if (value)
            updated_state |= flag;
        else
            updated_state &= ~flag;
        updated_state &= ~State::IsNonSilent;

        if (!m_state.compare_exchange_strong(state, updated_state))
            continue;

        if (has_flag(state, State::IsNonSilent))
            dispatch_output_state_change(false, transition_sequence_number(updated_state));
        return;
    }
}

void AudioOutputMonitor::dispatch_output_state_change(bool is_non_silent, u64 transition_sequence_number)
{
    m_main_thread_event_loop.deferred_invoke([self = NonnullRefPtr(*this), is_non_silent, transition_sequence_number] {
        if (transition_sequence_number <= self->m_last_delivered_transition_sequence_number)
            return;

        self->m_last_delivered_transition_sequence_number = transition_sequence_number;
        if (self->m_on_output_state_change)
            self->m_on_output_state_change(is_non_silent);
    });
}

}
