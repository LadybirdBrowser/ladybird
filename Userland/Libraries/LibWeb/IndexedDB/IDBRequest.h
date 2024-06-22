/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/EventTarget.h>

namespace Web::IndexedDB {

class IDBRequest : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(IDBRequest, DOM::EventTarget);
    JS_DECLARE_ALLOCATOR(IDBRequest);

public:
    virtual ~IDBRequest() override;

    void set_onsuccess(WebIDL::CallbackType*);
    WebIDL::CallbackType* onsuccess();
    void set_onerror(WebIDL::CallbackType*);
    WebIDL::CallbackType* onerror();

protected:
    explicit IDBRequest(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

}
