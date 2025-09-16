/*
 * Copyright (c) 2025,  Niccolo Antonelli-Dziri <niccolo.antonelli-dziri@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/NotificationPrototype.h>
#include <LibWeb/NotificationsAPI/Notification.h>

// FIXME: Remove this include when removing `dbgln` from `construct_impl`
#include <AK/Format.h>

namespace Web::NotificationsAPI {

GC_DEFINE_ALLOCATOR(Notification);

Notification::Notification(JS::Realm& realm)
    : DOM::EventTarget(realm)
{
}

// https://notifications.spec.whatwg.org/#constructors
WebIDL::ExceptionOr<GC::Ref<Notification>> Notification::construct_impl(
    JS::Realm& realm,
    String title,
    Optional<NotificationOptions> options)
{

    // FIXME: all of the steps specified in the spec

    // This is temporary to avoid the error: unused parameter
    dbgln(title);
    dbgln(options->body);

    return realm.create<Notification>(realm);
}

void Notification::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Notification);
    Base::initialize(realm);
}

}
