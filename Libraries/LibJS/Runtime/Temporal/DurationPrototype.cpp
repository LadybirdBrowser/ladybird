/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Temporal/Duration.h>
#include <LibJS/Runtime/Temporal/DurationPrototype.h>

namespace JS::Temporal {

GC_DEFINE_ALLOCATOR(DurationPrototype);

// 7.3 Properties of the Temporal.Duration Prototype Object, https://tc39.es/proposal-temporal/#sec-properties-of-the-temporal-duration-prototype-object
DurationPrototype::DurationPrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().object_prototype())
{
}

void DurationPrototype::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 7.3.2 Temporal.Duration.prototype[ %Symbol.toStringTag% ], https://tc39.es/proposal-temporal/#sec-temporal.duration.prototype-%symbol.tostringtag%
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "Temporal.Duration"_string), Attribute::Configurable);

#define __JS_ENUMERATE(unit) \
    define_native_accessor(realm, vm.names.unit, unit##_getter, {}, Attribute::Configurable);
    JS_ENUMERATE_DURATION_UNITS
#undef __JS_ENUMERATE
}

// 7.3.3 get Temporal.Duration.prototype.years, https://tc39.es/proposal-temporal/#sec-get-temporal.duration.prototype.years
// 7.3.4 get Temporal.Duration.prototype.months, https://tc39.es/proposal-temporal/#sec-get-temporal.duration.prototype.months
// 7.3.5 get Temporal.Duration.prototype.weeks, https://tc39.es/proposal-temporal/#sec-get-temporal.duration.prototype.weeks
// 7.3.6 get Temporal.Duration.prototype.days, https://tc39.es/proposal-temporal/#sec-get-temporal.duration.prototype.days
// 7.3.7 get Temporal.Duration.prototype.hours, https://tc39.es/proposal-temporal/#sec-get-temporal.duration.prototype.hours
// 7.3.8 get Temporal.Duration.prototype.minutes, https://tc39.es/proposal-temporal/#sec-get-temporal.duration.prototype.minutes
// 7.3.9 get Temporal.Duration.prototype.seconds, https://tc39.es/proposal-temporal/#sec-get-temporal.duration.prototype.seconds
// 7.3.10 get Temporal.Duration.prototype.milliseconds, https://tc39.es/proposal-temporal/#sec-get-temporal.duration.prototype.milliseconds
// 7.3.11 get Temporal.Duration.prototype.microseconds, https://tc39.es/proposal-temporal/#sec-get-temporal.duration.prototype.microseconds
// 7.3.12 get Temporal.Duration.prototype.nanoseconds, https://tc39.es/proposal-temporal/#sec-get-temporal.duration.prototype.nanoseconds
#define __JS_ENUMERATE(unit)                                                               \
    JS_DEFINE_NATIVE_FUNCTION(DurationPrototype::unit##_getter)                            \
    {                                                                                      \
        /* 1. Let duration be the this value. */                                           \
        /* 2. Perform ? RequireInternalSlot(duration, [[InitializedTemporalDuration]]). */ \
        auto duration = TRY(typed_this_object(vm));                                        \
                                                                                           \
        /* 3. Return 𝔽(duration.[[<unit>]]). */                                         \
        return duration->unit();                                                           \
    }
JS_ENUMERATE_DURATION_UNITS
#undef __JS_ENUMERATE

}
