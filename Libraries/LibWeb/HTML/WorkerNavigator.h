/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/GPC/GlobalPrivacyControl.h>
#include <LibWeb/HTML/NavigatorConcurrentHardware.h>
#include <LibWeb/HTML/NavigatorDeviceMemory.h>
#include <LibWeb/HTML/NavigatorID.h>
#include <LibWeb/HTML/NavigatorLanguage.h>
#include <LibWeb/HTML/NavigatorOnLine.h>
#include <LibWeb/MediaCapabilitiesAPI/MediaCapabilities.h>
#include <LibWeb/Serial/Serial.h>
#include <LibWeb/ServiceWorker/ServiceWorkerContainer.h>
#include <LibWeb/StorageAPI/NavigatorStorage.h>
#include <LibWeb/WebLocks/NavigatorLocks.h>

namespace Web::HTML {

class WorkerNavigator
    : public Bindings::GCAllocatedWrappable
    , public GlobalPrivacyControl::GlobalPrivacyControlMixin
    , public NavigatorConcurrentHardwareMixin
    , public NavigatorDeviceMemoryMixin
    , public NavigatorIDMixin
    , public NavigatorLanguageMixin
    , public NavigatorOnLineMixin
    , public StorageAPI::NavigatorStorage
    , public WebLocks::NavigatorLocks {
    WEB_WRAPPABLE(WorkerNavigator, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(WorkerNavigator);

public:
    [[nodiscard]] static GC::Ref<WorkerNavigator> create(WorkerGlobalScope&);

    GC::Ref<ServiceWorker::ServiceWorkerContainer> service_worker();

    virtual ~WorkerNavigator() override;

    GC::Ref<MediaCapabilitiesAPI::MediaCapabilities> media_capabilities();

    [[nodiscard]] GC::Ref<Serial::Serial> serial();

    [[nodiscard]] GC::Ref<PermissionsAPI::Permissions> permissions();

private:
    explicit WorkerNavigator(WorkerGlobalScope&);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    // ^StorageAPI::NavigatorStorage
    virtual EnvironmentSettingsObject& navigator_storage_settings_object() const override;

    GC::Ref<WorkerGlobalScope> m_global_scope;

    // ^WebLocks::NavigatorLocks
    virtual Bindings::Wrappable const& this_navigator_locks_object() const override { return *this; }
    virtual EnvironmentSettingsObject& this_navigator_locks_settings_object() const override { return navigator_storage_settings_object(); }

    // https://w3c.github.io/media-capabilities/#dom-workernavigator-mediacapabilities
    GC::Ptr<MediaCapabilitiesAPI::MediaCapabilities> m_media_capabilities;

    // https://wicg.github.io/serial/#extensions-to-the-workernavigator-interface
    GC::Ptr<Serial::Serial> m_serial;

    GC::Ptr<ServiceWorker::ServiceWorkerContainer> m_service_worker_container;

    // https://w3c.github.io/permissions/#navigator-and-workernavigator-extension
    GC::Ptr<PermissionsAPI::Permissions> m_permissions;
};

}
