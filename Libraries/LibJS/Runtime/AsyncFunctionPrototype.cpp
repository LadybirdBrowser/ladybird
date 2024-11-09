/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AsyncFunctionPrototype.h>
#include <LibJS/Runtime/GlobalObject.h>

namespace JS {

JS_DEFINE_ALLOCATOR(AsyncFunctionPrototype);

AsyncFunctionPrototype::AsyncFunctionPrototype(Realm& realm)
    : Object(ConstructWithPrototypeTag::Tag, realm.intrinsics().function_prototype())
{
}

void AsyncFunctionPrototype::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);

    // 27.7.3.2 AsyncFunction.prototype [ @@toStringTag ], https://tc39.es/ecma262/#sec-async-function-prototype-properties-toStringTag
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, vm.names.AsyncFunction.as_string()), Attribute::Configurable);
}

}
