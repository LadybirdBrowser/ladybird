/*
 * Copyright (c) 2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/IndexedDB/IDBCursor.h>

namespace Web::IndexedDB {

// https://w3c.github.io/IndexedDB/#idbcursorwithvalue
class IDBCursorWithValue : public IDBCursor {
    WEB_PLATFORM_OBJECT(IDBCursorWithValue, IDBCursor);
    GC_DECLARE_ALLOCATOR(IDBCursorWithValue);

public:
    virtual ~IDBCursorWithValue() override;

    // https://w3c.github.io/IndexedDB/#dom-idbcursorwithvalue-value
    [[nodiscard]] JS::Value value() { return m_value.value_or(JS::js_undefined()); }

private:
    explicit IDBCursorWithValue(JS::Realm&, CursorSourceHandle, GC::Ptr<Key>, Bindings::IDBCursorDirection, GotValue, GC::Ptr<Key>, JS::Value, GC::Ref<IDBKeyRange>, KeyOnly);
    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor& visitor) override;
};

}
