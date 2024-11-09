/*
 * Copyright (c) 2021, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AsyncIteratorPrototype.h>

namespace JS {

JS_DEFINE_ALLOCATOR(AsyncIteratorPrototype);

AsyncIteratorPrototype::AsyncIteratorPrototype(Realm& realm)
    : Object(ConstructWithPrototypeTag::Tag, realm.intrinsics().object_prototype())
{
}

void AsyncIteratorPrototype::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);
    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.well_known_symbol_async_iterator(), symbol_async_iterator, 0, attr);
}

// 27.1.3.1 %AsyncIteratorPrototype% [ @@asyncIterator ] ( ), https://tc39.es/ecma262/#sec-asynciteratorprototype-asynciterator
JS_DEFINE_NATIVE_FUNCTION(AsyncIteratorPrototype::symbol_async_iterator)
{
    // 1. Return the this value.
    return vm.this_value();
}

}
