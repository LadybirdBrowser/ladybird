/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/HTML/Scripting/ScriptAbortHandle.h>

namespace Web::HTML {

NonnullRefPtr<ScriptAbortHandle> ScriptAbortHandle::create(JS::VM& vm)
{
    return adopt_ref(*new ScriptAbortHandle(vm, Core::EventLoop::current_weak()));
}

ScriptAbortHandle::ScriptAbortHandle(JS::VM& vm, NonnullRefPtr<Core::WeakEventLoopReference> event_loop)
    : m_vm(&vm)
    , m_event_loop(move(event_loop))
{
}

ScriptAbortHandle::~ScriptAbortHandle() = default;

void ScriptAbortHandle::abort_script()
{
    MutexLocker locker(m_mutex);
    if (!m_vm)
        return;
    m_vm->request_interrupt();

    // NB: A nested event loop may be sleeping, with nothing to wake it up but this.
    if (auto event_loop = m_event_loop->take(); event_loop.is_alive())
        event_loop->wake();
}

void ScriptAbortHandle::detach()
{
    MutexLocker locker(m_mutex);
    m_vm = nullptr;
}

}
