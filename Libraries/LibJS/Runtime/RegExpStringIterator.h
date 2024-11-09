/*
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16View.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Utf16String.h>

namespace JS {

class RegExpStringIterator final : public Object {
    JS_OBJECT(RegExpStringIterator, Object);
    JS_DECLARE_ALLOCATOR(RegExpStringIterator);

public:
    static NonnullGCPtr<RegExpStringIterator> create(Realm&, Object& regexp_object, Utf16String string, bool global, bool unicode);

    virtual ~RegExpStringIterator() override = default;

    Object& regexp_object() { return m_regexp_object; }
    Utf16String string() const { return m_string; }
    bool global() const { return m_global; }
    bool unicode() const { return m_unicode; }

    bool done() const { return m_done; }
    void set_done() { m_done = true; }

private:
    explicit RegExpStringIterator(Object& prototype, Object& regexp_object, Utf16String string, bool global, bool unicode);

    virtual void visit_edges(Cell::Visitor&) override;

    NonnullGCPtr<Object> m_regexp_object;
    Utf16String m_string;
    bool m_global { false };
    bool m_unicode { false };
    bool m_done { false };
};

}
