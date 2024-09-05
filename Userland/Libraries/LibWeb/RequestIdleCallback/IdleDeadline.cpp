/*
 * Copyright (c) 2021, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/IdleDeadlinePrototype.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/RequestIdleCallback/IdleDeadline.h>

namespace Web::RequestIdleCallback {

JS_DEFINE_ALLOCATOR(IdleDeadline);

JS::NonnullGCPtr<IdleDeadline> IdleDeadline::create(JS::Realm& realm, bool did_timeout)
{
    return realm.heap().allocate<IdleDeadline>(realm, realm, did_timeout);
}

IdleDeadline::IdleDeadline(JS::Realm& realm, bool did_timeout)
    : PlatformObject(realm)
    , m_did_timeout(did_timeout)
{
}

void IdleDeadline::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(IdleDeadline);
}

IdleDeadline::~IdleDeadline() = default;

// https://w3c.github.io/requestidlecallback/#dom-idledeadline-timeremaining
double IdleDeadline::time_remaining() const
{
    auto const& event_loop = HTML::main_thread_event_loop();
    // 1. Let now be a DOMHighResTimeStamp representing current high resolution time in milliseconds.
    auto now = HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(*this));
    // 2. Let deadline be the result of calling IdleDeadline's get deadline time algorithm.
    auto deadline = event_loop.compute_deadline();
    // 3. Let timeRemaining be deadline - now.
    auto time_remaining = deadline - now;
    // 4. If timeRemaining is negative, set it to 0.
    if (time_remaining < 0)
        time_remaining = 0;
    // 5. Return timeRemaining.
    // NOTE: coarsening to milliseconds
    return ceil(time_remaining);
}

}
