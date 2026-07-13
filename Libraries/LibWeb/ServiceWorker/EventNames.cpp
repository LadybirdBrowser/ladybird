/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ServiceWorker/EventNames.h>

namespace Web::ServiceWorker::EventNames {

#define __ENUMERATE_SERVICE_WORKER_EVENT(name) \
    Utf16FlyString const& name = *new Utf16FlyString(#name##_utf16_fly_string);
ENUMERATE_SERVICE_WORKER_EVENTS
#undef __ENUMERATE_SERVICE_WORKER_EVENT

}
