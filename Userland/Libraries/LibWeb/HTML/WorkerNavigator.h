/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/HTML/NavigatorConcurrentHardware.h>
#include <LibWeb/HTML/NavigatorID.h>
#include <LibWeb/HTML/NavigatorLanguage.h>
#include <LibWeb/HTML/NavigatorOnLine.h>
#include <LibWeb/StorageAPI/NavigatorStorage.h>

namespace Web::HTML {

class WorkerNavigator : public Bindings::PlatformObject
    , public NavigatorConcurrentHardwareMixin
    , public NavigatorIDMixin
    , public NavigatorLanguageMixin
    , public NavigatorOnLineMixin
    , public StorageAPI::NavigatorStorage {
    WEB_PLATFORM_OBJECT(WorkerNavigator, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(WorkerNavigator);

public:
    [[nodiscard]] static JS::NonnullGCPtr<WorkerNavigator> create(WorkerGlobalScope&);

    virtual ~WorkerNavigator() override;

private:
    explicit WorkerNavigator(WorkerGlobalScope&);

    virtual void initialize(JS::Realm&) override;

    // ^StorageAPI::NavigatorStorage
    virtual Bindings::PlatformObject const& this_navigator_storage_object() const override { return *this; }
};

}
