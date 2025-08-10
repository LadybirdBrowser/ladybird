/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::StorageAPI {

class StorageManager final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(StorageManager, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(StorageManager);

public:
    static WebIDL::ExceptionOr<GC::Ref<StorageManager>> create(JS::Realm&);
    virtual ~StorageManager() override = default;

    GC::Ref<WebIDL::Promise> estimate() const;

private:
    StorageManager(JS::Realm&);

    virtual void initialize(JS::Realm&) override;

    static void queue_a_storage_task(JS::Realm&, JS::Object& global, Function<void()>);
    static GC::Ptr<StorageShelf> obtain_a_local_storage_shelf(HTML::EnvironmentSettingsObject&);
};

}
