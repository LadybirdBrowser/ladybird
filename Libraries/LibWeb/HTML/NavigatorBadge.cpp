/*
 * Copyright (c) 2025, Estefania Sanchez <e.snchez.c@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Navigator.h>
#include <LibWeb/HTML/NavigatorBadge.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::HTML {

// https://w3c.github.io/badging/#setting-the-application-badge
GC::Ref<WebIDL::Promise> NavigatorBadgeMixin::set_app_badge(Optional<u64> contents)
{
    // 1. Let global be context's relevant global object.
    auto& window_object = window();
    auto& realm = window_object.realm();

    // 2. If global is a Window object, then:
    // 2-1. Let document be global's associated Document.
    auto& document = window_object.associated_document();

    // 2-2. If document is not fully active, return a promise rejected with a "InvalidStateError" DOMException.
    if (!document.is_fully_active()) {
        auto exception = WebIDL::InvalidStateError::create(realm, "Document is not fully active"_utf16);
        return WebIDL::create_rejected_promise(realm, exception);
    }

    // 2-3. If document's relevant settings object's origin is not same origin-domain with this's relevant settings
    // object's top-level origin, return a promise rejected with a "SecurityError" DOMException.
    auto const document_origin = document.relevant_settings_object().origin();
    auto navigator = window_object.navigator();
    auto& this_settings = HTML::relevant_settings_object(*navigator);
    if (this_settings.top_level_origin.has_value() && !document_origin.is_same_origin_domain(this_settings.top_level_origin.value())) {
        auto exception = WebIDL::SecurityError::create(realm, "Document's origin is not same origin-domain with top-level origin"_utf16);
        return WebIDL::create_rejected_promise(realm, exception);
    }

    // 3. Let promise be a new promise.
    auto promise = WebIDL::create_promise(realm);

    // FIXME: 4. In parallel:
    // FIXME: 4-1. If the user agent requires express permission to set the application badge, then:
    // FIXME: 4-1-1. Let permissionState be the result of getting the current permission state with "notifications".
    // FIXME: 4-1-2. If permissionState is not "granted", queue a global task on the user interaction task source given
    // global to reject promise with a NotAllowedError and terminate this algorithm.

    // FIXME: 4-2. Switching on contents, if it happens to be the case that:
    // contents was not passed: Set badge to "flag".
    // contents is 0: Set badge to "nothing".
    // contents: Set badge to contents.
    (void)contents;

    // FIXME: 4-3. Queue a global task on the DOM manipulation task source given global to resolve promise with undefined.
    WebIDL::resolve_promise(realm, promise, JS::js_undefined());

    // 5. Return promise.
    return promise;
}

// https://w3c.github.io/badging/#clearappbadge-method
GC::Ref<WebIDL::Promise> NavigatorBadgeMixin::clear_app_badge()
{
    // When the clearAppBadge() method is called, the user agent MUST set the application badge of this to 0.
    return set_app_badge(0);
}

}
