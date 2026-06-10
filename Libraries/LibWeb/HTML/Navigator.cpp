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
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/Clipboard/Clipboard.h>
#include <LibWeb/CredentialManagement/CredentialsContainer.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/EncryptedMediaExtensions/Algorithms.h>
#include <LibWeb/EncryptedMediaExtensions/EncryptedMediaExtensions.h>
#include <LibWeb/EncryptedMediaExtensions/MediaKeySystemAccess.h>
#include <LibWeb/Geolocation/Geolocation.h>
#include <LibWeb/HTML/Navigator.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Internals/XRTest.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/MediaCapture/MediaDevices.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/PermissionsAPI/Permissions.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/ServiceWorker/ServiceWorkerContainer.h>
#include <LibWeb/WebXR/XRSystem.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(Navigator);

struct NavigatorBatteryPromiseCacheEntry {
    GC::Weak<Navigator> navigator;
    Bindings::WrapperWorldWeakValueCache<WebIDL::Promise> battery_promises;
};

static Vector<NavigatorBatteryPromiseCacheEntry>& navigator_battery_promise_caches()
{
    static NeverDestroyed<Vector<NavigatorBatteryPromiseCacheEntry>> caches;
    return *caches;
}

static void prune_navigator_battery_promise_caches()
{
    navigator_battery_promise_caches().remove_all_matching([](auto const& entry) {
        return !entry.navigator;
    });
}

static Bindings::WrapperWorldWeakValueCache<WebIDL::Promise>& battery_promise_cache_for(Navigator& navigator)
{
    auto& caches = navigator_battery_promise_caches();
    prune_navigator_battery_promise_caches();

    for (auto& entry : caches) {
        if (entry.navigator.ptr() == &navigator)
            return entry.battery_promises;
    }

    caches.append(NavigatorBatteryPromiseCacheEntry { navigator, {} });
    return caches.last().battery_promises;
}

GC::Ref<Navigator> Navigator::create(Window& window)
{
    return GC::Heap::the().allocate<Navigator>(window);
}

Navigator::Navigator(Window& window)
    : m_window(window)
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

GC::Ref<WebXR::XRSystem> Navigator::xr(JS::Realm& realm)
{
    if (!m_xr)
        m_xr = WebXR::XRSystem::create(*m_window);
    auto& wrapper_world = Bindings::host_defined_wrapper_world(realm);

    if (Window::is_internals_object_exposed())
        Bindings::wrap(wrapper_world, realm, GC::Ref { *m_xr })->define_direct_property("test"_utf16_fly_string, Bindings::wrap(wrapper_world, realm, Internals::XRTest::create(*m_window)), JS::default_attributes);

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
GC::Ref<WebIDL::Promise> Navigator::get_battery(JS::Realm& realm)
{
    auto& wrapper_world = Bindings::host_defined_wrapper_world(realm);
    auto& cache = battery_promise_cache_for(*this);

    // 1. If this.[[BatteryPromise]] is null, then set it to a new promise in this's relevant realm.
    if (auto battery_promise = cache.get(wrapper_world))
        return *battery_promise;

    auto battery_promise = WebIDL::create_promise(realm);
    cache.set(wrapper_world, battery_promise);

    start_get_battery_steps(battery_promise);

    // 4. Return this.[[BatteryPromise]].
    return battery_promise;
}

void Navigator::start_get_battery_steps(GC::Ref<WebIDL::Promise> promise)
{
    // 2. If this's relevant global object's associated Document is not allowed to use the "battery" policy-controlled
    //    feature, then reject this.[[BatteryPromise]] with a "NotAllowedError" DOMException.
    if (true) {
        auto& realm = WebIDL::promise_realm(promise);
        WebIDL::reject_promise(realm, promise, WebIDL::NotAllowedError::create("Battery Status API is not yet implemented"_utf16));
    }
    // 3. Otherwise:
    else {
        // FIXME: 1. If this.[[BatteryManager]] is null, then set it to the result of creating a new BatteryManager in this's
        //           relevant realm.
        // FIXME: 2. Resolve this.[[BatteryPromise]] with this.[[BatteryManager]].
    }
}

GC::Ref<PermissionsAPI::Permissions> Navigator::permissions()
{
    if (!m_permissions)
        m_permissions = PermissionsAPI::Permissions::create();
    return *m_permissions;
}

void Navigator::request_media_key_system_access(JS::Realm& realm, Utf16String key_system, Vector<EncryptedMediaExtensions::MediaKeySystemConfiguration> supported_configurations, GC::Ref<WebIDL::Promise> promise)
{
    // 1. If this's relevant global object's associated Document is not allowed to use the encrypted-media feature, then throw a "SecurityError" DOMException and abort these steps.
    auto& associated_document = window().associated_document();
    if (!associated_document.is_allowed_to_use_feature(DOM::PolicyControlledFeature::EncryptedMedia)) {
        WebIDL::reject_promise(realm, promise, WebIDL::SecurityError::create(realm, "This document is not allowed to use the encrypted-media feature"_utf16));
        return;
    }

    // 2. If keySystem is the empty string, return a promise rejected with a newly created TypeError.
    if (key_system.is_empty()) {
        WebIDL::reject_promise(realm, promise, JS::TypeError::create(realm, "keySystem must not be empty"_utf16));
        return;
    }

    // 3. If supportedConfigurations is empty, return a promise rejected with a newly created TypeError.
    if (supported_configurations.is_empty()) {
        WebIDL::reject_promise(realm, promise, JS::TypeError::create(realm, "supportedConfigurations must not be empty"_utf16));
        return;
    }

    // 4. Let document be the calling context's Document.
    // FIXME: Is this the same as the associated document?
    auto& document = associated_document;

    // 5. Let origin be the origin of document.
    auto origin = document.origin();

    // 7. Run the following steps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(GC::Heap::the(), [&realm, key_system, supported_configurations = move(supported_configurations), origin, promise]() mutable {
        TemporaryExecutionContext context(realm, TemporaryExecutionContext::CallbacksEnabled::Yes);

        // 1. If keySystem is not one of the Key Systems supported by the user agent, reject promise with a NotSupportedError. String comparison is case-sensitive.
        if (!EncryptedMediaExtensions::is_supported_key_system(key_system))
            return WebIDL::reject_promise(realm, promise, WebIDL::NotSupportedError::create(realm, "Unsupported key system"_utf16));

        // 2. Let implementation be the implementation of keySystem.
        auto implementation = EncryptedMediaExtensions::key_system_from_string(key_system);

        // 3. For each value in supportedConfigurations:
        for (auto const& candidate_configuration : supported_configurations) {
            // 1. Let candidate configuration be the value.

            // 2. Let supported configuration be the result of executing the Get Supported Configuration algorithm on implementation, candidate configuration, and origin.
            auto supported_configuration = EncryptedMediaExtensions::get_supported_configuration(*implementation, candidate_configuration, origin);

            // 3. If supported configuration is not NotSupported, run the following steps:
            if (supported_configuration.has_value()) {
                // 1. Let access be a new MediaKeySystemAccess object, and initialize it as follows:
                // 1. Set the keySystem attribute to keySystem.
                // 2. Let the configuration value be supported configuration.
                // 3. Let the cdm implementation value be implementation.
                auto access = EncryptedMediaExtensions::MediaKeySystemAccess::create(key_system, *supported_configuration->configuration, move(implementation));

                // 2. Resolve promise with access and abort the parallel steps of this algorithm.
                return WebIDL::resolve_promise(realm, promise, Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, access));
            }
        }

        // 4. Reject promise with a NotSupportedError.
        WebIDL::reject_promise(realm, promise, WebIDL::NotSupportedError::create(realm, "No supported configurations found for the requested key system"_utf16));
    }));
}

}
