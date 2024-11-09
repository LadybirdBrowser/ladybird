/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AggregateErrorPrototype.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/PrimitiveString.h>

namespace JS {

JS_DEFINE_ALLOCATOR(AggregateErrorPrototype);

AggregateErrorPrototype::AggregateErrorPrototype(Realm& realm)
    : Object(ConstructWithPrototypeTag::Tag, realm.intrinsics().error_prototype())
{
}

void AggregateErrorPrototype::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);
    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_direct_property(vm.names.name, PrimitiveString::create(vm, "AggregateError"_string), attr);
    define_direct_property(vm.names.message, PrimitiveString::create(vm, String {}), attr);
}

}
