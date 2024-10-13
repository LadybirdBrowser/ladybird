/*
 * Copyright (c) 2024, Brandon Gutzmann <brandgutz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::IndexedDB {

// https://w3c.github.io/IndexedDB/#idbobjectstore
class IDBObjectStore : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(IDBObjectStore, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(IDBObjectStore);

public:
    virtual ~IDBObjectStore() override;

protected:
    explicit IDBObjectStore(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

}
