/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ServiceWorker/ServiceWorkerGlobalScope.h>

namespace Web::ServiceWorker {

GC_DEFINE_ALLOCATOR(ServiceWorkerGlobalScope);

ServiceWorkerGlobalScope::~ServiceWorkerGlobalScope() = default;

ServiceWorkerGlobalScope::ServiceWorkerGlobalScope(JS::Realm& realm, GC::Ref<Web::Page> page)
    : HTML::WorkerGlobalScope(realm, page)
{
}

}
