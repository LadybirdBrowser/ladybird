/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Utf8View.h>
#include <LibJS/Runtime/Object.h>

namespace JS {

class StringIterator final : public Object {
    JS_OBJECT(StringIterator, Object);
    JS_DECLARE_ALLOCATOR(StringIterator);

public:
    static NonnullGCPtr<StringIterator> create(Realm&, String string);

    virtual ~StringIterator() override = default;

    Utf8CodePointIterator& iterator() { return m_iterator; }
    bool done() const { return m_done; }

private:
    explicit StringIterator(String string, Object& prototype);

    friend class StringIteratorPrototype;

    String m_string;
    Utf8CodePointIterator m_iterator;
    bool m_done { false };
};

}
