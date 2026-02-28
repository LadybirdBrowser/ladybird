/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf8View.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/StringIterator.h>

namespace JS {

GC_DEFINE_ALLOCATOR(StringIterator);

GC::Ref<StringIterator> StringIterator::create(Realm& realm, String string)
{
    return realm.create<StringIterator>(move(string), realm.intrinsics().string_iterator_prototype());
}

StringIterator::StringIterator(String string, Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
    , m_string(move(string))
    , m_iterator(Utf8View(m_string).begin())
{
}

BuiltinIterator* StringIterator::as_builtin_iterator_if_next_is_not_redefined(Value next_method)
{
    if (auto native_function = next_method.as_if<NativeFunction>()) {
        if (native_function->is_string_prototype_next_builtin())
            return this;
    }
    return nullptr;
}

ThrowCompletionOr<void> StringIterator::next(VM& vm, bool& done, Value& value)
{
    if (m_done) {
        done = true;
        value = js_undefined();
        return {};
    }

    if (m_iterator.done()) {
        m_done = true;
        done = true;
        value = js_undefined();
        return {};
    }

    auto code_point = String::from_code_point(*m_iterator);
    ++m_iterator;

    value = PrimitiveString::create(vm, move(code_point));
    return {};
}

}
