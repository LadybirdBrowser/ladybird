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
    JS_DECLARE_ALLOCATOR(StorageManager);

public:
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<StorageManager>> create(JS::Realm&);
    virtual ~StorageManager() override = default;

private:
    StorageManager(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

}
