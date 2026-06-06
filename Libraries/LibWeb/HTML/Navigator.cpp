/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Navigator.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/Bindings/XRSystem.h>
#include <LibWeb/Bindings/XRTest.h>
#include <LibWeb/Clipboard/Clipboard.h>
#include <LibWeb/CredentialManagement/CredentialsContainer.h>
#include <LibWeb/Geolocation/Geolocation.h>
#include <LibWeb/HTML/Navigator.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Internals/XRTest.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/MediaCapture/MediaDevices.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/PermissionsAPI/Permissions.h>
#include <LibWeb/ServiceWorker/ServiceWorkerContainer.h>
#include <LibWeb/WebXR/XRSystem.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(Navigator);

GC::Ref<Navigator> Navigator::create(Window& window)
{
    return GC::Heap::the().allocate<Navigator>(window);
}

Navigator::Navigator(Window& window)
    : Bindings::Wrappable()
    , m_window(window)
{
    NavigatorGamepadPartial::check_for_connected_gamepads();
}

Navigator::~Navigator() = default;

EnvironmentSettingsObject& Navigator::navigator_storage_settings_object() const
{
    return relevant_settings_object(*m_window);
}

// https://html.spec.whatwg.org/multipage/system-state.html#dom-navigator-pdfviewerenabled
bool Navigator::pdf_viewer_enabled() const
{
    // The NavigatorPlugins mixin's pdfViewerEnabled getter steps are to return the user agent's PDF viewer supported.
    // NOTE: The NavigatorPlugins mixin should only be exposed on the Window object.
    return m_window->page().pdf_viewer_supported();
}

// https://w3c.github.io/webdriver/#dfn-webdriver
bool Navigator::webdriver() const
{
    // Returns true if webdriver-active flag is set, false otherwise.

    // NOTE: The NavigatorAutomationInformation interface should not be exposed on WorkerNavigator.
    return m_window->page().is_webdriver_active();
}

void Navigator::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    NavigatorGamepadPartial::visit_edges(visitor);
    visitor.visit(m_window);
    visitor.visit(m_mime_type_array);
    visitor.visit(m_plugin_array);
    visitor.visit(m_clipboard);
    visitor.visit(m_geolocation);
    visitor.visit(m_serial);
    visitor.visit(m_user_activation);
    visitor.visit(m_service_worker_container);
    visitor.visit(m_media_capabilities);
    visitor.visit(m_media_devices);
    visitor.visit(m_credentials);
    visitor.visit(m_battery_promise);
    visitor.visit(m_xr);
    visitor.visit(m_permissions);
}

GC::Ref<MimeTypeArray> Navigator::mime_types()
{
    if (!m_mime_type_array)
        m_mime_type_array = MimeTypeArray::create(*m_window);
    return *m_mime_type_array;
}

GC::Ref<PluginArray> Navigator::plugins()
{
    if (!m_plugin_array)
        m_plugin_array = PluginArray::create(*m_window);
    return *m_plugin_array;
}

GC::Ref<Clipboard::Clipboard> Navigator::clipboard()
{
    if (!m_clipboard)
        m_clipboard = Clipboard::Clipboard::create();
    return *m_clipboard;
}

GC::Ref<Geolocation::Geolocation> Navigator::geolocation()
{
    if (!m_geolocation)
        m_geolocation = Geolocation::Geolocation::create(*m_window);
    return *m_geolocation;
}

GC::Ref<Serial::Serial> Navigator::serial()
{
    if (!m_serial)
        m_serial = Serial::Serial::create();
    return *m_serial;
}

GC::Ref<UserActivation> Navigator::user_activation()
{
    if (!m_user_activation)
        m_user_activation = UserActivation::create(*m_window);
    return *m_user_activation;
}

GC::Ref<CredentialManagement::CredentialsContainer> Navigator::credentials()
{
    if (!m_credentials)
        m_credentials = CredentialManagement::CredentialsContainer::create();
    return *m_credentials;
}

GC::Ref<WebXR::XRSystem> Navigator::xr()
{
    if (!m_xr) {
        auto& realm = m_window->realm();
        m_xr = WebXR::XRSystem::create(*m_window);
        if (Window::is_internals_object_exposed()) {
            auto& wrapper_world = Bindings::host_defined_wrapper_world(realm);
            Bindings::wrap(wrapper_world, realm, m_xr)->define_direct_property("test"_utf16_fly_string, Bindings::wrap(wrapper_world, realm, Internals::XRTest::create(*m_window)), JS::default_attributes);
        }
    }
    return *m_xr;
}

// https://w3c.github.io/pointerevents/#dom-navigator-maxtouchpoints
WebIDL::Long Navigator::max_touch_points()
{
    // FIXME: Implement this for touch-capable devices.
    return 0;
}

GC::Ref<ServiceWorker::ServiceWorkerContainer> Navigator::service_worker()
{
    if (!m_service_worker_container)
        m_service_worker_container = ServiceWorker::ServiceWorkerContainer::create(relevant_settings_object(*m_window));
    return *m_service_worker_container;
}

GC::Ref<MediaCapabilitiesAPI::MediaCapabilities> Navigator::media_capabilities()
{
    if (!m_media_capabilities)
        m_media_capabilities = MediaCapabilitiesAPI::MediaCapabilities::create();
    return *m_media_capabilities;
}

GC::Ref<MediaCapture::MediaDevices> Navigator::media_devices()
{
    if (!m_media_devices)
        m_media_devices = MediaCapture::MediaDevices::create(*m_window);
    return *m_media_devices;
}

// https://w3c.github.io/battery/#the-getbattery-method
GC::Ref<WebIDL::Promise> Navigator::get_battery()
{
    auto& realm = m_window->realm();

    // 1. If this.[[BatteryPromise]] is null, then set it to a new promise in this's relevant realm.
    if (!m_battery_promise)
        m_battery_promise = WebIDL::create_promise(realm);

    // 2. If this's relevant global object's associated Document is not allowed to use the "battery" policy-controlled
    //    feature, then reject this.[[BatteryPromise]] with a "NotAllowedError" DOMException.
    if (true) {
        WebIDL::reject_promise(realm, *m_battery_promise, WebIDL::NotAllowedError::create(realm, "Battery Status API is not yet implemented"_utf16));
    }
    // 3. Otherwise:
    else {
        // FIXME: 1. If this.[[BatteryManager]] is null, then set it to the result of creating a new BatteryManager in this's
        //           relevant realm.
        // FIXME: 2. Resolve this.[[BatteryPromise]] with this.[[BatteryManager]].
    }

    // 4. Return this.[[BatteryPromise]].
    return *m_battery_promise;
}

GC::Ref<PermissionsAPI::Permissions> Navigator::permissions()
{
    if (!m_permissions)
        m_permissions = PermissionsAPI::Permissions::create();
    return *m_permissions;
}

}
