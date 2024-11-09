/*
 * Copyright (c) 2024, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/ServiceWorkerRegistrationPrototype.h>
#include <LibWeb/HTML/ServiceWorkerRegistration.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(ServiceWorkerRegistration);

ServiceWorkerRegistration::ServiceWorkerRegistration(JS::Realm& realm)
    : DOM::EventTarget(realm)
{
}

void ServiceWorkerRegistration::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ServiceWorkerRegistration);
}

JS::NonnullGCPtr<ServiceWorkerRegistration> ServiceWorkerRegistration::create(JS::Realm& realm)
{
    return realm.heap().allocate<ServiceWorkerRegistration>(realm, realm);
}
}
