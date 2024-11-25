/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/Temporal/Instant.h>
#include <LibJS/Runtime/Temporal/Now.h>
#include <LibJS/Runtime/Temporal/PlainDate.h>
#include <LibJS/Runtime/Temporal/PlainDateTime.h>
#include <LibJS/Runtime/Temporal/PlainTime.h>
#include <LibJS/Runtime/Temporal/TimeZone.h>
#include <LibJS/Runtime/Temporal/ZonedDateTime.h>

namespace JS::Temporal {

GC_DEFINE_ALLOCATOR(Now);

// 2 The Temporal.Now Object, https://tc39.es/proposal-temporal/#sec-temporal-now-object
Now::Now(Realm& realm)
    : Object(ConstructWithPrototypeTag::Tag, realm.intrinsics().object_prototype())
{
}

void Now::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 2.1.1 Temporal.Now [ %Symbol.toStringTag% ], https://tc39.es/proposal-temporal/#sec-temporal-now-%symbol.tostringtag%
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "Temporal.Now"_string), Attribute::Configurable);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.timeZoneId, time_zone_id, 0, attr);
    define_native_function(realm, vm.names.instant, instant, 0, attr);
    define_native_function(realm, vm.names.plainDateTimeISO, plain_date_time_iso, 0, attr);
    define_native_function(realm, vm.names.zonedDateTimeISO, zoned_date_time_iso, 0, attr);
    define_native_function(realm, vm.names.plainDateISO, plain_date_iso, 0, attr);
    define_native_function(realm, vm.names.plainTimeISO, plain_time_iso, 0, attr);
}

// 2.2.1 Temporal.Now.timeZoneId ( ), https://tc39.es/proposal-temporal/#sec-temporal.now.timezoneid
JS_DEFINE_NATIVE_FUNCTION(Now::time_zone_id)
{
    // 1. Return SystemTimeZoneIdentifier().
    return PrimitiveString::create(vm, system_time_zone_identifier());
}

// 2.2.2 Temporal.Now.instant ( ), https://tc39.es/proposal-temporal/#sec-temporal.now.instant
JS_DEFINE_NATIVE_FUNCTION(Now::instant)
{
    // 1. Let ns be SystemUTCEpochNanoseconds().
    auto nanoseconds = system_utc_epoch_nanoseconds(vm);

    // 2. Return ! CreateTemporalInstant(ns).
    return MUST(create_temporal_instant(vm, BigInt::create(vm, move(nanoseconds))));
}

// 2.2.3 Temporal.Now.plainDateTimeISO ( [ temporalTimeZoneLike ] ), https://tc39.es/proposal-temporal/#sec-temporal.now.plaindatetimeiso
JS_DEFINE_NATIVE_FUNCTION(Now::plain_date_time_iso)
{
    auto temporal_time_zone_like = vm.argument(0);

    // 1. Let isoDateTime be ? SystemDateTime(temporalTimeZoneLike).
    auto iso_date_time = TRY(system_date_time(vm, temporal_time_zone_like));

    // 2. Return ! CreateTemporalDateTime(isoDateTime, "iso8601").
    return MUST(create_temporal_date_time(vm, iso_date_time, "iso8601"_string));
}

// 2.2.4 Temporal.Now.zonedDateTimeISO ( [ temporalTimeZoneLike ] ), https://tc39.es/proposal-temporal/#sec-temporal.now.zoneddatetimeiso
JS_DEFINE_NATIVE_FUNCTION(Now::zoned_date_time_iso)
{
    auto temporal_time_zone_like = vm.argument(0);
    String time_zone;

    // 1. If temporalTimeZoneLike is undefined, then
    if (temporal_time_zone_like.is_undefined()) {
        // a. Let timeZone be SystemTimeZoneIdentifier().
        time_zone = system_time_zone_identifier();
    }
    // 2. Else,
    else {
        // a. Let timeZone be ? ToTemporalTimeZoneIdentifier(temporalTimeZoneLike).
        time_zone = TRY(to_temporal_time_zone_identifier(vm, temporal_time_zone_like));
    }

    //  3. Let ns be SystemUTCEpochNanoseconds().
    auto nanoseconds = system_utc_epoch_nanoseconds(vm);

    //  4. Return ! CreateTemporalZonedDateTime(ns, timeZone, "iso8601").
    return MUST(create_temporal_zoned_date_time(vm, BigInt::create(vm, move(nanoseconds)), move(time_zone), "iso8601"_string));
}

// 2.2.5 Temporal.Now.plainDateISO ( [ temporalTimeZoneLike ] ), https://tc39.es/proposal-temporal/#sec-temporal.now.plaindateiso
JS_DEFINE_NATIVE_FUNCTION(Now::plain_date_iso)
{
    auto temporal_time_zone_like = vm.argument(0);

    // 1. Let isoDateTime be ? SystemDateTime(temporalTimeZoneLike).
    auto iso_date_time = TRY(system_date_time(vm, temporal_time_zone_like));

    // 2. Return ! CreateTemporalDate(isoDateTime.[[ISODate]], "iso8601").
    return MUST(create_temporal_date(vm, iso_date_time.iso_date, "iso8601"_string));
}

// 2.2.6 Temporal.Now.plainTimeISO ( [ temporalTimeZoneLike ] ), https://tc39.es/proposal-temporal/#sec-temporal.now.plaintimeiso
JS_DEFINE_NATIVE_FUNCTION(Now::plain_time_iso)
{
    auto temporal_time_zone_like = vm.argument(0);

    // 1. Let isoDateTime be ? SystemDateTime(temporalTimeZoneLike).
    auto iso_date_time = TRY(system_date_time(vm, temporal_time_zone_like));

    // 2. Return ! CreateTemporalTime(isoDateTime.[[Time]]).
    return MUST(create_temporal_time(vm, iso_date_time.time));
}

// 2.3.3 SystemUTCEpochNanoseconds ( ), https://tc39.es/proposal-temporal/#sec-temporal-systemutcepochnanoseconds
Crypto::SignedBigInteger system_utc_epoch_nanoseconds(VM& vm)
{
    // 1. Let global be GetGlobalObject().
    auto const& global = vm.get_global_object();

    // 2. Let nowNs be HostSystemUTCEpochNanoseconds(global).
    auto now_ns = vm.host_system_utc_epoch_nanoseconds(global);

    // 3. Return ℤ(nowNs).
    return now_ns;
}

// 2.3.4 SystemDateTime ( temporalTimeZoneLike ), https://tc39.es/proposal-temporal/#sec-temporal-systemdatetime
ThrowCompletionOr<ISODateTime> system_date_time(VM& vm, Value temporal_time_zone_like)
{
    String time_zone;

    // 1. If temporalTimeZoneLike is undefined, then
    if (temporal_time_zone_like.is_undefined()) {
        // a. Let timeZone be SystemTimeZoneIdentifier().
        time_zone = system_time_zone_identifier();
    }
    // 2. Else,
    else {
        // a. Let timeZone be ? ToTemporalTimeZoneIdentifier(temporalTimeZoneLike).
        time_zone = TRY(to_temporal_time_zone_identifier(vm, temporal_time_zone_like));
    }

    // 3. Let epochNs be SystemUTCEpochNanoseconds().
    auto epoch_nanoseconds = system_utc_epoch_nanoseconds(vm);

    // 4. Return GetISODateTimeFor(timeZone, epochNs).
    return get_iso_date_time_for(time_zone, epoch_nanoseconds);
}

}
