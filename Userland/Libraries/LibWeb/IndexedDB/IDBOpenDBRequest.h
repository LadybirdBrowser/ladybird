/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/IndexedDB/IDBRequest.h>

namespace Web::IndexedDB {

// https://w3c.github.io/IndexedDB/#idbopendbrequest
class IDBOpenDBRequest : public IDBRequest {
    WEB_PLATFORM_OBJECT(IDBOpenDBRequest, IDBRequest);
    JS_DECLARE_ALLOCATOR(IDBOpenDBRequest);

public:
    virtual ~IDBOpenDBRequest();

    void set_onblocked(WebIDL::CallbackType*);
    WebIDL::CallbackType* onblocked();
    void set_onupgradeneeded(WebIDL::CallbackType*);
    WebIDL::CallbackType* onupgradeneeded();

protected:
    explicit IDBOpenDBRequest(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

}
