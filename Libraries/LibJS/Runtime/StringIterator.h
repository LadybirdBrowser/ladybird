/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/Object.h>

namespace JS {

class StringIterator final : public Object
    , public BuiltinIterator {
    JS_OBJECT(StringIterator, Object);
    GC_DECLARE_ALLOCATOR(StringIterator);

public:
    static GC::Ref<StringIterator> create(Realm&, Utf16String string);

    virtual ~StringIterator() override = default;

    BuiltinIterator* as_builtin_iterator_if_next_is_not_redefined(Value next_method) override;
    ThrowCompletionOr<void> next(VM&, bool& done, Value& value) override;

private:
    explicit StringIterator(Utf16String string, Object& prototype);

    friend class StringIteratorPrototype;

    Utf16String m_string;
    AK::Utf16CodePointIterator m_iterator;
    bool m_done { false };
};

}
