/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/WorkerGlobalScope.h>

namespace Web::ServiceWorker {

// https://w3c.github.io/ServiceWorker/#serviceworkerglobalscope
class ServiceWorkerGlobalScope : public HTML::WorkerGlobalScope {
    WEB_PLATFORM_OBJECT(ServiceWorkerGlobalScope, HTML::WorkerGlobalScope);
    GC_DECLARE_ALLOCATOR(ServiceWorkerGlobalScope);

public:
    virtual ~ServiceWorkerGlobalScope() override;

protected:
    explicit ServiceWorkerGlobalScope(JS::Realm&, GC::Ref<Web::Page>);
};

}
