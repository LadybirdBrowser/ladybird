/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Temporal/DurationConstructor.h>
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

    // 1.1.1 Temporal [ %Symbol.toStringTag% ], https://tc39.es/proposal-temporal/#sec-temporal-%symbol.tostringtag%
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "Temporal"_string), Attribute::Configurable);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_intrinsic_accessor(vm.names.Duration, attr, [](auto& realm) -> Value { return realm.intrinsics().temporal_duration_constructor(); });
}

}
