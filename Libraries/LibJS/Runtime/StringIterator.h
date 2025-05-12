/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Utf8View.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/Object.h>

namespace JS {

class StringIterator final : public Object
    , public BuiltinIterator {
    JS_OBJECT(StringIterator, Object);
    GC_DECLARE_ALLOCATOR(StringIterator);

public:
    static GC::Ref<StringIterator> create(Realm&, String string);

    virtual ~StringIterator() override = default;

    BuiltinIterator* as_builtin_iterator_if_next_is_not_redefined() override
    {
        if (m_next_method_was_redefined)
            return nullptr;
        return this;
    }
    ThrowCompletionOr<void> next(VM&, bool& done, Value& value) override;

private:
    explicit StringIterator(String string, Object& prototype);

    friend class StringIteratorPrototype;

    String m_string;
    Utf8CodePointIterator m_iterator;
    bool m_done { false };
};

}
