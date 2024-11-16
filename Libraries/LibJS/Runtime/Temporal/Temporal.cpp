/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Temporal/Temporal.h>

namespace JS::Temporal {

GC_DEFINE_ALLOCATOR(Temporal);

// 1 The Temporal Object, https://tc39.es/proposal-temporal/#sec-temporal-objects
Temporal::Temporal(Realm& realm)
    : Object(ConstructWithPrototypeTag::Tag, realm.intrinsics().object_prototype())
{
}

void Temporal::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 1.1.1 Temporal [ @@toStringTag ], https://tc39.es/proposal-temporal/#sec-temporal-@@tostringtag
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "Temporal"_string), Attribute::Configurable);
}

}
