/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/HTML/NavigatorConcurrentHardware.h>
#include <LibWeb/HTML/NavigatorDeviceMemory.h>
#include <LibWeb/HTML/NavigatorID.h>
#include <LibWeb/HTML/NavigatorLanguage.h>
#include <LibWeb/HTML/NavigatorOnLine.h>
#include <LibWeb/HTML/ServiceWorkerContainer.h>
#include <LibWeb/MediaCapabilitiesAPI/MediaCapabilities.h>
#include <LibWeb/StorageAPI/NavigatorStorage.h>

namespace Web::HTML {

class WorkerNavigator : public Bindings::PlatformObject
    , public NavigatorConcurrentHardwareMixin
    , public NavigatorDeviceMemoryMixin
    , public NavigatorIDMixin
    , public NavigatorLanguageMixin
    , public NavigatorOnLineMixin
    , public StorageAPI::NavigatorStorage {
    WEB_PLATFORM_OBJECT(WorkerNavigator, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(WorkerNavigator);

public:
    [[nodiscard]] static GC::Ref<WorkerNavigator> create(WorkerGlobalScope&);

    GC::Ref<ServiceWorkerContainer> service_worker();

    virtual ~WorkerNavigator() override;

    GC::Ref<MediaCapabilitiesAPI::MediaCapabilities> media_capabilities();

private:
    explicit WorkerNavigator(WorkerGlobalScope&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    // ^StorageAPI::NavigatorStorage
    virtual Bindings::PlatformObject const& this_navigator_storage_object() const override { return *this; }

    // https://w3c.github.io/media-capabilities/#dom-workernavigator-mediacapabilities
    GC::Ptr<MediaCapabilitiesAPI::MediaCapabilities> m_media_capabilities;

    GC::Ptr<ServiceWorkerContainer> m_service_worker_container;
};

}
