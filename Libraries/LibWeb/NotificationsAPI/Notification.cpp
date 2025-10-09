/*
 * Copyright (c) 2025, Niccolo Antonelli-Dziri <niccolo.antonelli-dziri@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/NotificationPrototype.h>
#include <LibWeb/NotificationsAPI/Notification.h>

namespace Web::NotificationsAPI {

GC_DEFINE_ALLOCATOR(Notification);

Notification::Notification(JS::Realm& realm)
    : DOM::EventTarget(realm)
{
}

// https://notifications.spec.whatwg.org/#constructors
WebIDL::ExceptionOr<GC::Ref<Notification>> Notification::construct_impl(
    JS::Realm& realm,
    String const&,                 // FIXME: title is unused
    Optional<NotificationOptions>) // FIXME: options is unused
{

    // FIXME: all of the steps specified in the spec

    return realm.create<Notification>(realm);
}

void Notification::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Notification);
    Base::initialize(realm);
}

}
