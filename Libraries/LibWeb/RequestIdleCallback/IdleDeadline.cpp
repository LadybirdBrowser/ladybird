/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/RequestIdleCallback/IdleDeadline.h>

namespace Web::RequestIdleCallback {

extern "C" {
// Defined in Bindings/Rust/src/request_idle_callback/idle_deadline.rs.
void* web_rust_idle_deadline_create(JSVM*, bool did_timeout);

// Called by Bindings/Rust/src/request_idle_callback/idle_deadline.rs.
double web_request_idle_callback_idle_deadline_deadline(JSObject* global_object);
}

GC::Ref<IdleDeadline> IdleDeadline::create(bool did_timeout)
{
    return GC::Ref { *static_cast<IdleDeadline*>(web_rust_idle_deadline_create(JS::HostABI::vm_to_abi(JS::VM::the()), did_timeout)) };
}

double web_request_idle_callback_idle_deadline_deadline(JSObject* global_object)
{
    // NB: The event loop deadline uses the shared monotonic clock, while now is relative to the global's time origin.
    return HighResolutionTime::relative_high_resolution_time(HTML::main_thread_event_loop().compute_deadline(), *JS::HostABI::object_from_abi(global_object));
}

}
