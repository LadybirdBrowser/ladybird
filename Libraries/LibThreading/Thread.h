/*
 * Copyright (c) 2019-2020, Sergey Bugaev <bugaevc@serenityos.org>
 * Copyright (c) 2021, Spencer Dixon <spencercdixon@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/AtomicRefCounted.h>
#include <AK/ByteString.h>
#include <AK/DistinctNumeric.h>
#include <AK/Function.h>
#include <AK/Result.h>
#include <LibCore/EventReceiver.h>
#include <pthread.h>

namespace Threading {

AK_TYPEDEF_DISTINCT_ORDERED_ID(intptr_t, ThreadError);

// States of userspace threads are simplified over actual kernel states (and possibly POSIX states).
// There are only a couple of well-defined transitions between these states, and any attempt to call a function in a state where this is not allowed will crash the program.
enum class ThreadState : u8 {
    // Thread has been constructed but not started.
    // Transitions to Running via start().
    Startable,
    // Thread has been started, might be running, and can be joined.
    // Note that join() (valid to call in this state) only changes the thread state after the thread has exited, so it only ever transitions from Exited to Joined.
    // Transitions to Detached via detach(), transitions to Exited when the thread finishes its action function.
    Running,
    // Thread has not been detached and exited, and has to still be joined.
    // Transitions to Joined via join().
    Exited,
    // Thread has been started but also detached, meaning it cannot be joined.
    // Transitions to DetachedExited when the thread finishes its action function.
    Detached,
    // Thread has exited but was detached, meaning it cannot be joined.
    DetachedExited,
    // Thread has exited and been joined.
    Joined,
};

class Thread final
    : public AtomicRefCounted<Thread>
    , public Weakable<Thread> {
public:
    static NonnullRefPtr<Thread> construct(ESCAPING Function<intptr_t()> action, StringView thread_name = {})
    {
        return adopt_ref(*new Thread(move(action), thread_name));
    }
    static ErrorOr<NonnullRefPtr<Thread>> try_create(ESCAPING Function<intptr_t()> action, StringView thread_name = {})
    {
        return adopt_nonnull_ref_or_enomem(new (nothrow) Thread(move(action), thread_name));
    }

    virtual ~Thread();

    ErrorOr<void> set_priority(int priority);
    ErrorOr<int> get_priority() const;

    // Only callable in the Startable state.
    void start();
    // Only callable in the Running state.
    void detach();

    // Only callable in the Running or Exited states.
    template<typename T = void>
    Result<T, ThreadError> join();

    ByteString thread_name() const;
    pthread_t tid() const;
    ThreadState state() const;
    bool is_started() const;
    bool needs_to_be_joined() const;
    bool has_exited() const;

private:
    explicit Thread(ESCAPING Function<intptr_t()> action, StringView thread_name = {});
    Function<intptr_t()> m_action;
    pthread_t m_tid { 0 };
    ByteString m_thread_name;
    Atomic<ThreadState> m_state { ThreadState::Startable };
};

template<typename T>
Result<T, ThreadError> Thread::join()
{
    VERIFY(needs_to_be_joined());

    void* thread_return = nullptr;
    int rc = pthread_join(m_tid, &thread_return);
    if (rc != 0) {
        return ThreadError { rc };
    }

    // The other thread has now stopped running, so a TOCTOU bug is not possible.
    // (If you call join from two different threads, you're doing something *very* wrong anyways.)
    VERIFY(m_state == ThreadState::Exited);
    m_state = ThreadState::Joined;

    if constexpr (IsVoid<T>)
        return {};
    else
        return { static_cast<T>(thread_return) };
}

}

template<>
struct AK::Formatter<Threading::Thread> : AK::Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, Threading::Thread const& thread)
    {
        return Formatter<FormatString>::format(builder, "Thread \"{}\"({})"sv, thread.thread_name(), thread.tid());
    }
};

template<>
struct AK::Formatter<Threading::ThreadState> : AK::Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, Threading::ThreadState state)
    {
        ByteString name = "";
        switch (state) {
        case Threading::ThreadState::Detached:
            name = "Detached";
            break;
        case Threading::ThreadState::DetachedExited:
            name = "DetachedExited";
            break;
        case Threading::ThreadState::Exited:
            name = "Exited";
            break;
        case Threading::ThreadState::Joined:
            name = "Joined";
            break;
        case Threading::ThreadState::Running:
            name = "Running";
            break;
        case Threading::ThreadState::Startable:
            name = "Startable";
            break;
        default:
            VERIFY_NOT_REACHED();
        }
        return Formatter<FormatString>::format(builder, "{}"sv, name);
    }
};
