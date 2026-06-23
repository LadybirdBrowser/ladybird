/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibJS/Forward.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/EventLoop/Task.h>

namespace Web::WebLocks {

// https://w3c.github.io/web-locks/#lock-request-queue
using LockRequestQueue = Vector<GC::Ref<LockRequest>>;

// https://w3c.github.io/web-locks/#lock-manager-lock-request-queue-map
using LockRequestQueueMap = HashMap<String, LockRequestQueue>;

HTML::ParallelQueue& lock_task_queue();
void queue_web_locks_task(JS::Realm&, GC::Ref<GC::Function<void()>> steps);
void queue_web_locks_task_on_relevant_event_loop(JS::Realm&, GC::Ref<WebIDL::CallbackType> callback, GC::Ref<GC::Function<void()>> steps);

LockManager& obtain_lock_manager(HTML::EnvironmentSettingsObject&);

LockRequestQueue& get_lock_request_queue(LockRequestQueueMap&, String const& name);

}
