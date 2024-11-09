/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Temporal/CalendarConstructor.h>
#include <LibJS/Runtime/Temporal/DurationConstructor.h>
#include <LibJS/Runtime/Temporal/InstantConstructor.h>
#include <LibJS/Runtime/Temporal/Now.h>
#include <LibJS/Runtime/Temporal/PlainDateConstructor.h>
#include <LibJS/Runtime/Temporal/PlainDateTimeConstructor.h>
#include <LibJS/Runtime/Temporal/PlainMonthDayConstructor.h>
#include <LibJS/Runtime/Temporal/PlainTimeConstructor.h>
#include <LibJS/Runtime/Temporal/PlainYearMonthConstructor.h>
#include <LibJS/Runtime/Temporal/Temporal.h>
#include <LibJS/Runtime/Temporal/TimeZoneConstructor.h>
#include <LibJS/Runtime/Temporal/ZonedDateTimeConstructor.h>

namespace JS::Temporal {

JS_DEFINE_ALLOCATOR(Temporal);

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

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_direct_property(vm.names.Now, heap().allocate<Now>(realm, realm), attr);
    define_intrinsic_accessor(vm.names.Calendar, attr, [](auto& realm) -> Value { return realm.intrinsics().temporal_calendar_constructor(); });
    define_intrinsic_accessor(vm.names.Duration, attr, [](auto& realm) -> Value { return realm.intrinsics().temporal_duration_constructor(); });
    define_intrinsic_accessor(vm.names.Instant, attr, [](auto& realm) -> Value { return realm.intrinsics().temporal_instant_constructor(); });
    define_intrinsic_accessor(vm.names.PlainDate, attr, [](auto& realm) -> Value { return realm.intrinsics().temporal_plain_date_constructor(); });
    define_intrinsic_accessor(vm.names.PlainDateTime, attr, [](auto& realm) -> Value { return realm.intrinsics().temporal_plain_date_time_constructor(); });
    define_intrinsic_accessor(vm.names.PlainMonthDay, attr, [](auto& realm) -> Value { return realm.intrinsics().temporal_plain_month_day_constructor(); });
    define_intrinsic_accessor(vm.names.PlainTime, attr, [](auto& realm) -> Value { return realm.intrinsics().temporal_plain_time_constructor(); });
    define_intrinsic_accessor(vm.names.PlainYearMonth, attr, [](auto& realm) -> Value { return realm.intrinsics().temporal_plain_year_month_constructor(); });
    define_intrinsic_accessor(vm.names.TimeZone, attr, [](auto& realm) -> Value { return realm.intrinsics().temporal_time_zone_constructor(); });
    define_intrinsic_accessor(vm.names.ZonedDateTime, attr, [](auto& realm) -> Value { return realm.intrinsics().temporal_zoned_date_time_constructor(); });
}

}
