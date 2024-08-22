/*
 * Copyright (c) 2024, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/ServiceWorkerContainerPrototype.h>
#include <LibWeb/HTML/ServiceWorkerContainer.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(ServiceWorkerContainer);

ServiceWorkerContainer::ServiceWorkerContainer(JS::Realm& realm)
    : DOM::EventTarget(realm)
{
}

void ServiceWorkerContainer::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ServiceWorkerContainer);
}

JS::NonnullGCPtr<ServiceWorkerContainer> ServiceWorkerContainer::create(JS::Realm& realm)
{
    return realm.heap().allocate<ServiceWorkerContainer>(realm, realm);
}

}
