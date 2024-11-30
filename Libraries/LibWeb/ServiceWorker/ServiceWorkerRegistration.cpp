/*
 * Copyright (c) 2024, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/ServiceWorkerRegistrationPrototype.h>
#include <LibWeb/ServiceWorker/ServiceWorkerRegistration.h>

namespace Web::ServiceWorker {

GC_DEFINE_ALLOCATOR(ServiceWorkerRegistration);

ServiceWorkerRegistration::ServiceWorkerRegistration(JS::Realm& realm)
    : DOM::EventTarget(realm)
{
}

void ServiceWorkerRegistration::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ServiceWorkerRegistration);
}

GC::Ref<ServiceWorkerRegistration> ServiceWorkerRegistration::create(JS::Realm& realm)
{
    return realm.create<ServiceWorkerRegistration>(realm);
}
}
