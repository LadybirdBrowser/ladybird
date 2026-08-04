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
    WEB_WRAPPABLE(IDBCursorWithValue, IDBCursor);
    GC_DECLARE_ALLOCATOR(IDBCursorWithValue);

public:
    virtual ~IDBCursorWithValue() override;

    [[nodiscard]] Optional<JS::Value> const& current_value() const { return m_value; }
    [[nodiscard]] JS::Value value() const { return m_value.value_or(JS::js_undefined()); }

private:
    explicit IDBCursorWithValue(CursorSourceHandle, GC::Ptr<Key>, CursorDirection, GotValue, GC::Ptr<Key>, JS::Value, GC::Ref<IDBKeyRange>, KeyOnly);
};

}
