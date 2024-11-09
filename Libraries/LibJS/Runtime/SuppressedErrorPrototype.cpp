/*
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/SuppressedErrorPrototype.h>

namespace JS {

JS_DEFINE_ALLOCATOR(SuppressedErrorPrototype);

SuppressedErrorPrototype::SuppressedErrorPrototype(Realm& realm)
    : Object(ConstructWithPrototypeTag::Tag, realm.intrinsics().error_prototype())
{
}

void SuppressedErrorPrototype::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);
    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_direct_property(vm.names.name, PrimitiveString::create(vm, "SuppressedError"_string), attr);
    define_direct_property(vm.names.message, PrimitiveString::create(vm, String {}), attr);
}

}
