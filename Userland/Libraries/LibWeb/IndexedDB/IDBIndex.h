/*
 * Copyright (c) 2024, Brandon Gutzmann <brandgutz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::IndexedDB {

// https://w3c.github.io/IndexedDB/#idbindex
class IDBIndex : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(IDBIndex, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(IDBIndex);

public:
    virtual ~IDBIndex() override;

protected:
    explicit IDBIndex(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

}
