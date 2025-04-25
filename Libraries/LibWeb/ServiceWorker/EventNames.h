/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>

namespace Web::ServiceWorker::EventNames {

#define ENUMERATE_SERVICE_WORKER_EVENTS        \
    __ENUMERATE_SERVICE_WORKER_EVENT(activate) \
    __ENUMERATE_SERVICE_WORKER_EVENT(fetch)    \
    __ENUMERATE_SERVICE_WORKER_EVENT(install)  \
    __ENUMERATE_SERVICE_WORKER_EVENT(message)  \
    __ENUMERATE_SERVICE_WORKER_EVENT(messageerror)

#define __ENUMERATE_SERVICE_WORKER_EVENT(name) extern FlyString name;
ENUMERATE_SERVICE_WORKER_EVENTS
#undef __ENUMERATE_SERVICE_WORKER_EVENT

}
