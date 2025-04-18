/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf8View.h>
#include <LibJS/Runtime/GlobalObject.h>
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

}
