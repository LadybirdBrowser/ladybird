/*
 * Copyright (c) 2024, Brandon Gutzmann <brandgutz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::IndexedDB {

// https://w3c.github.io/IndexedDB/#idbcursor
class IDBCursor : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(IDBCursor, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(IDBCursor);

public:
    virtual ~IDBCursor() override;

protected:
    explicit IDBCursor(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

}
