/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibURL/URL.h>
#include <LibWeb/Bindings/ServiceWorkerPrototype.h>
#include <LibWeb/Bindings/WorkerPrototype.h>

namespace Web::ServiceWorker {

// https://w3c.github.io/ServiceWorker/#dfn-service-worker
// This class corresponds to "service worker", not "ServiceWorker"
// FIXME: This should be owned and managed at the user agent level
// FIXME: A lot of the fields for this struct actually need to live in the Agent for the service worker in the WebWorker process
struct ServiceWorker {
    Bindings::ServiceWorkerState state = Bindings::ServiceWorkerState::Parsed; // https://w3c.github.io/ServiceWorker/#dfn-state
    URL::URL script_url;                                                       // https://w3c.github.io/ServiceWorker/#dfn-script-url
    Bindings::WorkerType worker_type = Bindings::WorkerType::Classic;          // https://w3c.github.io/ServiceWorker/#dfn-type

    // FIXME: A lot more fields after this...
};

}
