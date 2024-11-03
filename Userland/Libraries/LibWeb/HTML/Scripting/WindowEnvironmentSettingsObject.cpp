/*
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HostDefined.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Scripting/WindowEnvironmentSettingsObject.h>
#include <LibWeb/HTML/Window.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(WindowEnvironmentSettingsObject);

WindowEnvironmentSettingsObject::WindowEnvironmentSettingsObject(Window& window, NonnullOwnPtr<JS::ExecutionContext> execution_context)
    : EnvironmentSettingsObject(move(execution_context))
    , m_window(window)
{
}

WindowEnvironmentSettingsObject::~WindowEnvironmentSettingsObject() = default;

void WindowEnvironmentSettingsObject::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_window);
}

// https://html.spec.whatwg.org/multipage/window-object.html#set-up-a-window-environment-settings-object
void WindowEnvironmentSettingsObject::setup(Page& page, URL::URL const& creation_url, NonnullOwnPtr<JS::ExecutionContext> execution_context, JS::GCPtr<Environment> reserved_environment, URL::URL top_level_creation_url, URL::Origin top_level_origin)
{
    // 1. Let realm be the value of execution context's Realm component.
    auto realm = execution_context->realm;
    VERIFY(realm);

    // 2. Let window be realm's global object.
    auto& window = verify_cast<HTML::Window>(realm->global_object());

    // 3. Let settings object be a new environment settings object whose algorithms are defined as follows:
    // NOTE: See the functions defined for this class.
    auto settings_object = realm->heap().allocate<WindowEnvironmentSettingsObject>(*realm, window, move(execution_context));

    // 4. If reservedEnvironment is non-null, then:
    if (reserved_environment) {
        // FIXME:    1. Set settings object's id to reservedEnvironment's id,
        //              target browsing context to reservedEnvironment's target browsing context,
        //              and active service worker to reservedEnvironment's active service worker.
        settings_object->id = reserved_environment->id;
        settings_object->target_browsing_context = reserved_environment->target_browsing_context;

        // 2. Set reservedEnvironment's id to the empty string.
        reserved_environment->id = String {};
    }

    // 5. Otherwise, ...
    else {
        // FIXME: ...set settings object's id to a new unique opaque string,
        //        settings object's target browsing context to null,
        //        and settings object's active service worker to null.
        static i64 next_id = 1;
        settings_object->id = String::number(next_id++);
        settings_object->target_browsing_context = nullptr;
    }

    // 6. Set settings object's creation URL to creationURL,
    //    settings object's top-level creation URL to topLevelCreationURL,
    //    and settings object's top-level origin to topLevelOrigin.
    settings_object->creation_url = creation_url;
    settings_object->top_level_creation_url = move(top_level_creation_url);
    settings_object->top_level_origin = move(top_level_origin);

    // 7. Set realm's [[HostDefined]] field to settings object.
    // Non-Standard: We store the ESO next to the web intrinsics in a custom HostDefined object
    auto intrinsics = realm->heap().allocate<Bindings::Intrinsics>(*realm, *realm);
    auto host_defined = make<Bindings::HostDefined>(settings_object, intrinsics, page);
    realm->set_host_defined(move(host_defined));

    // Non-Standard: We cannot fully initialize window object until *after* the we set up
    //    the realm's [[HostDefined]] internal slot as the internal slot contains the web platform intrinsics
    MUST(window.initialize_web_interfaces({}));
}

// https://html.spec.whatwg.org/multipage/window-object.html#script-settings-for-window-objects:responsible-document
JS::GCPtr<DOM::Document> WindowEnvironmentSettingsObject::responsible_document()
{
    // Return window's associated Document.
    return m_window->associated_document();
}

// https://html.spec.whatwg.org/multipage/window-object.html#script-settings-for-window-objects:api-url-character-encoding
String WindowEnvironmentSettingsObject::api_url_character_encoding()
{
    // Return the current character encoding of window's associated Document.
    return m_window->associated_document().encoding_or_default();
}

// https://html.spec.whatwg.org/multipage/window-object.html#script-settings-for-window-objects:api-base-url
URL::URL WindowEnvironmentSettingsObject::api_base_url()
{
    // Return the current base URL of window's associated Document.
    return m_window->associated_document().base_url();
}

// https://html.spec.whatwg.org/multipage/window-object.html#script-settings-for-window-objects:concept-settings-object-origin
URL::Origin WindowEnvironmentSettingsObject::origin()
{
    // Return the origin of window's associated Document.
    return m_window->associated_document().origin();
}

// https://html.spec.whatwg.org/multipage/window-object.html#script-settings-for-window-objects:concept-settings-object-policy-container
PolicyContainer WindowEnvironmentSettingsObject::policy_container()
{
    // Return the policy container of window's associated Document.
    return m_window->associated_document().policy_container();
}

// https://html.spec.whatwg.org/multipage/window-object.html#script-settings-for-window-objects:concept-settings-object-cross-origin-isolated-capability
CanUseCrossOriginIsolatedAPIs WindowEnvironmentSettingsObject::cross_origin_isolated_capability()
{
    // FIXME: Return true if both of the following hold, and false otherwise:
    //          1. realm's agent cluster's cross-origin-isolation mode is "concrete", and
    //          2. window's associated Document is allowed to use the "cross-origin-isolated" feature.
    return CanUseCrossOriginIsolatedAPIs::Yes;
}

}
