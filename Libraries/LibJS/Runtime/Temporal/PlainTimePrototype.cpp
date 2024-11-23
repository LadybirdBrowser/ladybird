/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Temporal/PlainTimePrototype.h>

namespace JS::Temporal {

GC_DEFINE_ALLOCATOR(PlainTimePrototype);

// 4.3 Properties of the Temporal.PlainTime Prototype Object, https://tc39.es/proposal-temporal/#sec-properties-of-the-temporal-plaintime-prototype-object
PlainTimePrototype::PlainTimePrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().object_prototype())
{
}

void PlainTimePrototype::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 4.3.2 Temporal.PlainTime.prototype[ %Symbol.toStringTag% ], https://tc39.es/proposal-temporal/#sec-temporal.plaintime.prototype-%symbol.tostringtag%
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "Temporal.PlainTime"_string), Attribute::Configurable);

    define_native_accessor(realm, vm.names.hour, hour_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.minute, minute_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.second, second_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.millisecond, millisecond_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.microsecond, microsecond_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.nanosecond, nanosecond_getter, {}, Attribute::Configurable);
}

// 4.3.3 get Temporal.PlainTime.prototype.hour, https://tc39.es/proposal-temporal/#sec-get-temporal.plaintime.prototype.hour
// 4.3.4 get Temporal.PlainTime.prototype.minute, https://tc39.es/proposal-temporal/#sec-get-temporal.plaintime.prototype.minute
// 4.3.5 get Temporal.PlainTime.prototype.second, https://tc39.es/proposal-temporal/#sec-get-temporal.plaintime.prototype.second
// 4.3.6 get Temporal.PlainTime.prototype.millisecond, https://tc39.es/proposal-temporal/#sec-get-temporal.plaintime.prototype.millisecond
// 4.3.7 get Temporal.PlainTime.prototype.microsecond, https://tc39.es/proposal-temporal/#sec-get-temporal.plaintime.prototype.microsecond
// 4.3.8 get Temporal.PlainTime.prototype.nanosecond, https://tc39.es/proposal-temporal/#sec-get-temporal.plaintime.prototype.microsecond
#define JS_ENUMERATE_PLAIN_TIME_FIELDS \
    __JS_ENUMERATE(hour)               \
    __JS_ENUMERATE(minute)             \
    __JS_ENUMERATE(second)             \
    __JS_ENUMERATE(millisecond)        \
    __JS_ENUMERATE(microsecond)        \
    __JS_ENUMERATE(nanosecond)

#define __JS_ENUMERATE(field)                                                              \
    JS_DEFINE_NATIVE_FUNCTION(PlainTimePrototype::field##_getter)                          \
    {                                                                                      \
        /* 1. Let temporalTime be the this value. */                                       \
        /* 2. Perform ? RequireInternalSlot(temporalTime, [[InitializedTemporalTime]]). */ \
        auto temporal_time = TRY(typed_this_object(vm));                                   \
                                                                                           \
        /* 3. Return 𝔽(temporalTime.[[Time]].[[<field>]]). */                           \
        return temporal_time->time().field;                                                \
    }
JS_ENUMERATE_PLAIN_TIME_FIELDS
#undef __JS_ENUMERATE

}
