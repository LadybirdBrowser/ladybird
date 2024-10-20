/*
 * Copyright (c) 2024, Brandon Gutzmann <brandgutz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/EventTarget.h>

namespace Web::IndexedDB {

// https://w3c.github.io/IndexedDB/#idbdatabase
class IDBDatabase : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(IDBDatabase, DOM::EventTarget);
    JS_DECLARE_ALLOCATOR(IDBDatabase);

public:
    virtual ~IDBDatabase() override;

protected:
    explicit IDBDatabase(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

}
