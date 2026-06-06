/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/StorageManager.h>
#include <LibWeb/Bindings/Wrappable.h>

namespace Web::StorageAPI {

class StorageManager final : public Bindings::Wrappable {
    WEB_WRAPPABLE(StorageManager, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(StorageManager);

public:
    static WebIDL::ExceptionOr<GC::Ref<StorageManager>> create(JS::Realm&);
    virtual ~StorageManager() override = default;

private:
    StorageManager(JS::Realm&);
};

}
