/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/EncryptedMediaExtensions/NavigatorEncryptedMediaExtensionsPartial.h>
#include <LibWeb/Forward.h>
#include <LibWeb/GPC/GlobalPrivacyControl.h>
#include <LibWeb/Gamepad/NavigatorGamepad.h>
#include <LibWeb/HTML/MimeTypeArray.h>
#include <LibWeb/HTML/NavigatorBeacon.h>
#include <LibWeb/HTML/NavigatorConcurrentHardware.h>
#include <LibWeb/HTML/NavigatorDeviceMemory.h>
#include <LibWeb/HTML/NavigatorID.h>
#include <LibWeb/HTML/NavigatorLanguage.h>
#include <LibWeb/HTML/NavigatorOnLine.h>
#include <LibWeb/HTML/PluginArray.h>
#include <LibWeb/HTML/UserActivation.h>
#include <LibWeb/MediaCapabilitiesAPI/MediaCapabilities.h>
#include <LibWeb/MediaCapture/MediaDevices.h>
#include <LibWeb/Serial/Serial.h>
#include <LibWeb/StorageAPI/NavigatorStorage.h>

namespace Web::HTML {

class Navigator
    : public Bindings::Wrappable
    , public NavigatorBeaconPartial
    , public NavigatorConcurrentHardwareMixin
    , public NavigatorDeviceMemoryMixin
    , public Gamepad::NavigatorGamepadPartial
    , public GlobalPrivacyControl::GlobalPrivacyControlMixin
    , public EncryptedMediaExtensions::NavigatorEncryptedMediaExtensionsPartial
    , public NavigatorIDMixin
    , public NavigatorLanguageMixin
    , public NavigatorOnLineMixin
    , public StorageAPI::NavigatorStorage {
    WEB_WRAPPABLE(Navigator, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(Navigator);

public:
    [[nodiscard]] static GC::Ref<Navigator> create(Window&);

    // FIXME: Implement NavigatorContentUtilsMixin

    // NavigatorCookies
    // FIXME: Hook up to Agent level state
    // https://html.spec.whatwg.org/multipage/system-state.html#dom-navigator-cookieenabled
    bool cookie_enabled() const { return true; }

    // NavigatorPlugins
    // https://html.spec.whatwg.org/multipage/system-state.html#dom-navigator-javaenabled
    bool java_enabled() const { return false; }

    bool pdf_viewer_enabled() const;

    bool webdriver() const;

    Window& window() const { return *m_window; }

    [[nodiscard]] GC::Ref<MimeTypeArray> mime_types();
    [[nodiscard]] GC::Ref<PluginArray> plugins();
    [[nodiscard]] GC::Ref<Clipboard::Clipboard> clipboard();
    [[nodiscard]] GC::Ref<Geolocation::Geolocation> geolocation();
    [[nodiscard]] GC::Ref<Serial::Serial> serial();
    [[nodiscard]] GC::Ref<UserActivation> user_activation();
    [[nodiscard]] GC::Ref<CredentialManagement::CredentialsContainer> credentials();
    GC::Ref<WebIDL::Promise> get_battery(JS::Realm&);
    void start_get_battery_steps(GC::Ref<WebIDL::Promise>);
    [[nodiscard]] GC::Ref<WebXR::XRSystem> xr(JS::Realm&);
    [[nodiscard]] GC::Ref<PermissionsAPI::Permissions> permissions();
    void request_media_key_system_access(JS::Realm&, Utf16String key_system, Vector<EncryptedMediaExtensions::MediaKeySystemConfiguration> supported_configurations, GC::Ref<WebIDL::Promise>);

    GC::Ref<ServiceWorker::ServiceWorkerContainer> service_worker();

    GC::Ref<MediaCapabilitiesAPI::MediaCapabilities> media_capabilities();
    GC::Ref<MediaCapture::MediaDevices> media_devices();

    static WebIDL::Long max_touch_points();

    virtual ~Navigator() override;

protected:
    virtual void visit_edges(GC::Cell::Visitor&) override;

private:
    explicit Navigator(Window&);

    // ^StorageAPI::NavigatorStorage
    virtual EnvironmentSettingsObject& navigator_storage_settings_object() const override;

    GC::Ref<Window> m_window;

    GC::Ptr<PluginArray> m_plugin_array;
    GC::Ptr<MimeTypeArray> m_mime_type_array;

    // https://w3c.github.io/clipboard-apis/#dom-navigator-clipboard
    GC::Ptr<Clipboard::Clipboard> m_clipboard;

    // https://w3c.github.io/geolocation/#navigator_interface
    GC::Ptr<Geolocation::Geolocation> m_geolocation;

    // https://wicg.github.io/serial/#extensions-to-the-navigator-interface
    GC::Ptr<Serial::Serial> m_serial;

    // https://html.spec.whatwg.org/multipage/interaction.html#dom-navigator-useractivation
    GC::Ptr<UserActivation> m_user_activation;

    // https://w3c.github.io/ServiceWorker/#navigator-serviceworker
    GC::Ptr<ServiceWorker::ServiceWorkerContainer> m_service_worker_container;

    // https://w3c.github.io/media-capabilities/#dom-navigator-mediacapabilities
    GC::Ptr<MediaCapabilitiesAPI::MediaCapabilities> m_media_capabilities;

    // https://w3c.github.io/mediacapture-main/#dom-navigator-mediadevices
    GC::Ptr<MediaCapture::MediaDevices> m_media_devices;

    // https://w3c.github.io/webappsec-credential-management/#framework-credential-management
    GC::Ptr<CredentialManagement::CredentialsContainer> m_credentials;

    // https://immersive-web.github.io/webxr/#dom-navigator-xr
    GC::Ptr<WebXR::XRSystem> m_xr;

    // https://w3c.github.io/permissions/#navigator-and-workernavigator-extension
    GC::Ptr<PermissionsAPI::Permissions> m_permissions;
};

}
