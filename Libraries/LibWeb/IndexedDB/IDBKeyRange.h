/*
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::IndexedDB {

// https://w3c.github.io/IndexedDB/#keyrange
class IDBKeyRange : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(IDBKeyRange, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(IDBKeyRange);

public:
    virtual ~IDBKeyRange() override;
    [[nodiscard]] static GC::Ref<IDBKeyRange> create(JS::Realm&);

protected:
    explicit IDBKeyRange(JS::Realm&);
    virtual void initialize(JS::Realm&) override;
};

}
