/*
 * Copyright (c) 2024, Brandon Gutzmann <brandgutz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::IndexedDB {

// https://w3c.github.io/IndexedDB/#idbkeyrange
class IDBKeyRange : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(IDBKeyRange, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(IDBKeyRange);

public:
    virtual ~IDBKeyRange() override;

protected:
    explicit IDBKeyRange(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

}
