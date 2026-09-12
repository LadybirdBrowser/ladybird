/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/AtomicRefCounted.h>
#include <AK/EnumBits.h>
#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <LibCore/Forward.h>
#include <LibMedia/Export.h>

namespace Audio {

class MEDIA_API AudioOutputMonitor final : public AtomicRefCounted<AudioOutputMonitor> {
public:
    static NonnullRefPtr<AudioOutputMonitor> create(Core::EventLoop&);

    void set_output_state_change_handler(Function<void(bool)>);
    void set_enabled(bool);
    void set_muted(bool);
    void update(bool output_is_non_silent);

private:
    static constexpr u8 STATE_FLAG_BIT_COUNT = 3;

    enum class State : u64 {
        None = 0,
        Enabled = 1 << 0,
        Muted = 1 << 1,
        IsNonSilent = 1 << 2,
        FlagMask = (1 << STATE_FLAG_BIT_COUNT) - 1,
        TransitionSequenceIncrement = 1 << STATE_FLAG_BIT_COUNT,
    };
    AK_ENUM_BITWISE_FRIEND_OPERATORS(State);

    explicit AudioOutputMonitor(Core::EventLoop&);

    static State advance_transition_sequence_number(State);
    static u64 transition_sequence_number(State);

    void update_configuration(State flag, bool value);
    void dispatch_output_state_change(bool is_non_silent, u64 transition_sequence_number);

    Core::EventLoop& m_main_thread_event_loop;

    // These are only accessed on the main thread.
    Function<void(bool)> m_on_output_state_change;
    u64 m_last_delivered_transition_sequence_number { 0 };

    Atomic<State> m_state { State::None };
};

}
