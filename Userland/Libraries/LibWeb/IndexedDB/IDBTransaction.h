/*
 * Copyright (c) 2024, Brandon Gutzmann <brandgutz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/EventTarget.h>

namespace Web::IndexedDB {

// https://w3c.github.io/IndexedDB/#idbtransaction
class IDBTransaction : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(IDBTransaction, DOM::EventTarget);
    JS_DECLARE_ALLOCATOR(IDBTransaction);

public:
    virtual ~IDBTransaction() override;

protected:
    explicit IDBTransaction(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

}
