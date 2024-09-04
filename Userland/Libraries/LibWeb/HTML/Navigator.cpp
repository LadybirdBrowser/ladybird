/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Heap/Heap.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/NavigatorPrototype.h>
#include <LibWeb/Clipboard/Clipboard.h>
#include <LibWeb/HTML/Navigator.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/ServiceWorkerContainer.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/Page/Page.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(Navigator);

JS::NonnullGCPtr<Navigator> Navigator::create(JS::Realm& realm)
{
    return realm.heap().allocate<Navigator>(realm, realm);
}

Navigator::Navigator(JS::Realm& realm)
    : PlatformObject(realm)
{
}

Navigator::~Navigator() = default;

void Navigator::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Navigator);
}

// https://html.spec.whatwg.org/multipage/system-state.html#dom-navigator-pdfviewerenabled
bool Navigator::pdf_viewer_enabled() const
{
    // The NavigatorPlugins mixin's pdfViewerEnabled getter steps are to return the user agent's PDF viewer supported.
    // NOTE: The NavigatorPlugins mixin should only be exposed on the Window object.
    auto const& window = verify_cast<HTML::Window>(HTML::current_global_object());
    return window.page().pdf_viewer_supported();
}

// https://w3c.github.io/webdriver/#dfn-webdriver
bool Navigator::webdriver() const
{
    // Returns true if webdriver-active flag is set, false otherwise.

    // NOTE: The NavigatorAutomationInformation interface should not be exposed on WorkerNavigator.
    auto const& window = verify_cast<HTML::Window>(HTML::current_global_object());
    return window.page().is_webdriver_active();
}

void Navigator::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_mime_type_array);
    visitor.visit(m_plugin_array);
    visitor.visit(m_clipboard);
    visitor.visit(m_user_activation);
    visitor.visit(m_service_worker_container);
    visitor.visit(m_media_capabilities);
}

JS::NonnullGCPtr<MimeTypeArray> Navigator::mime_types()
{
    if (!m_mime_type_array)
        m_mime_type_array = heap().allocate<MimeTypeArray>(realm(), realm());
    return *m_mime_type_array;
}

JS::NonnullGCPtr<PluginArray> Navigator::plugins()
{
    if (!m_plugin_array)
        m_plugin_array = heap().allocate<PluginArray>(realm(), realm());
    return *m_plugin_array;
}

JS::NonnullGCPtr<Clipboard::Clipboard> Navigator::clipboard()
{
    if (!m_clipboard)
        m_clipboard = heap().allocate<Clipboard::Clipboard>(realm(), realm());
    return *m_clipboard;
}

JS::NonnullGCPtr<UserActivation> Navigator::user_activation()
{
    if (!m_user_activation)
        m_user_activation = heap().allocate<UserActivation>(realm(), realm());
    return *m_user_activation;
}

// https://w3c.github.io/pointerevents/#dom-navigator-maxtouchpoints
WebIDL::Long Navigator::max_touch_points()
{
    dbgln("FIXME: Unimplemented Navigator.maxTouchPoints");
    return 0;
}

// https://www.w3.org/TR/tracking-dnt/#dom-navigator-donottrack
Optional<FlyString> Navigator::do_not_track() const
{
    // The value is null if no DNT header field would be sent (e.g., because a tracking preference is not
    // enabled and no user-granted exception is applicable); otherwise, the value is a string beginning with
    // "0" or "1", possibly followed by DNT-extension characters.
    if (ResourceLoader::the().enable_do_not_track())
        return "1"_fly_string;

    return {};
}

JS::NonnullGCPtr<ServiceWorkerContainer> Navigator::service_worker()
{
    if (!m_service_worker_container)
        m_service_worker_container = heap().allocate<ServiceWorkerContainer>(realm(), realm());
    return *m_service_worker_container;
}

JS::NonnullGCPtr<MediaCapabilitiesAPI::MediaCapabilities> Navigator::media_capabilities()
{
    if (!m_media_capabilities)
        m_media_capabilities = heap().allocate<MediaCapabilitiesAPI::MediaCapabilities>(realm(), realm());
    return *m_media_capabilities;
}

}
