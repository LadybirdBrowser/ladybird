/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The Temporal.Now object and the system time operations.

use ak::Utf16String;
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::big_int::{BigInt, SignedBigInteger};
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::date::system_time_zone_identifier;
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::runtime::temporal::calendar::ISO8601_CALENDAR;
use crate::runtime::temporal::instant::create_temporal_instant;
use crate::runtime::temporal::iso_records::ISODateTime;
use crate::runtime::temporal::plain_date::create_temporal_date;
use crate::runtime::temporal::plain_date_time::create_temporal_date_time;
use crate::runtime::temporal::plain_time::create_temporal_time;
use crate::runtime::temporal::time_zone::{get_iso_date_time_for, to_temporal_time_zone_identifier};
use crate::runtime::temporal::zoned_date_time::create_temporal_zoned_date_time;
use crate::utf16::Utf16View;

// 2 The Temporal.Now Object, https://tc39.es/proposal-temporal/#sec-temporal-now-object
#[repr(C)]
#[derive(Trace)]
pub struct Now {
    base: Object,
}

define_object_class!(Now, extends: [Object], methods: {
    initialize: Now::initialize,
    ..ORDINARY_OBJECT_METHODS
});

impl Now {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<Now> {
        realm.create_object(
            vm,
            Now {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.object_prototype(),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let names = &vm.names;

        // 2.1.1 Temporal.Now [ %Symbol.toStringTag% ], https://tc39.es/proposal-temporal/#sec-temporal-now-%symbol.tostringtag%
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_utf8(vm, "Temporal.Now")),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );

        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        let define_native_function = |name: &PropertyKey, function, length| {
            object.define_native_function(vm, realm, name, function, length, attr, None);
        };
        define_native_function(&names.timeZoneId, raw_native!(Now::time_zone_id), 0);
        define_native_function(&names.instant, raw_native!(Now::instant), 0);
        define_native_function(&names.plainDateTimeISO, raw_native!(Now::plain_date_time_iso), 0);
        define_native_function(&names.zonedDateTimeISO, raw_native!(Now::zoned_date_time_iso), 0);
        define_native_function(&names.plainDateISO, raw_native!(Now::plain_date_iso), 0);
        define_native_function(&names.plainTimeISO, raw_native!(Now::plain_time_iso), 0);
    }

    // 2.2.1 Temporal.Now.timeZoneId ( ), https://tc39.es/proposal-temporal/#sec-temporal.now.timezoneid
    #[allow(clippy::unnecessary_wraps, reason = "native functions can throw")]
    fn time_zone_id(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Return SystemTimeZoneIdentifier().
        let time_zone = system_time_zone_identifier();
        Ok(Value::from_string(PrimitiveString::create(vm, time_zone)))
    }

    // 2.2.2 Temporal.Now.instant ( ), https://tc39.es/proposal-temporal/#sec-temporal.now.instant
    #[allow(clippy::unnecessary_wraps, reason = "native functions can throw")]
    fn instant(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let ns be SystemUTCEpochNanoseconds().
        let nanoseconds = system_utc_epoch_nanoseconds(vm);

        // 2. Return ! CreateTemporalInstant(ns).
        Ok(Value::from_object(
            create_temporal_instant(vm, BigInt::create(vm, nanoseconds), None).must(),
        ))
    }

    // 2.2.3 Temporal.Now.plainDateTimeISO ( [ temporalTimeZoneLike ] ), https://tc39.es/proposal-temporal/#sec-temporal.now.plaindatetimeiso
    fn plain_date_time_iso(vm: &Vm) -> ThrowCompletionOr<Value> {
        let temporal_time_zone_like = vm.argument(0);

        // 1. Let isoDateTime be ? SystemDateTime(temporalTimeZoneLike).
        let iso_date_time = system_date_time(vm, temporal_time_zone_like)?;

        // 2. Return ! CreateTemporalDateTime(isoDateTime, "iso8601").
        Ok(Value::from_object(
            create_temporal_date_time(vm, &iso_date_time, Utf16String::from_utf8(ISO8601_CALENDAR), None).must(),
        ))
    }

    // 2.2.4 Temporal.Now.zonedDateTimeISO ( [ temporalTimeZoneLike ] ), https://tc39.es/proposal-temporal/#sec-temporal.now.zoneddatetimeiso
    fn zoned_date_time_iso(vm: &Vm) -> ThrowCompletionOr<Value> {
        let temporal_time_zone_like = vm.argument(0);

        // 1. If temporalTimeZoneLike is undefined, then
        let time_zone = if temporal_time_zone_like.is_undefined() {
            // a. Let timeZone be SystemTimeZoneIdentifier().
            system_time_zone_identifier()
        }
        // 2. Else,
        else {
            // a. Let timeZone be ? ToTemporalTimeZoneIdentifier(temporalTimeZoneLike).
            to_temporal_time_zone_identifier(vm, temporal_time_zone_like)?
        };

        //  3. Let ns be SystemUTCEpochNanoseconds().
        let nanoseconds = system_utc_epoch_nanoseconds(vm);

        //  4. Return ! CreateTemporalZonedDateTime(ns, timeZone, "iso8601").
        Ok(Value::from_object(
            create_temporal_zoned_date_time(
                vm,
                BigInt::create(vm, nanoseconds),
                time_zone,
                Utf16String::from_utf8(ISO8601_CALENDAR),
                None,
            )
            .must(),
        ))
    }

    // 2.2.5 Temporal.Now.plainDateISO ( [ temporalTimeZoneLike ] ), https://tc39.es/proposal-temporal/#sec-temporal.now.plaindateiso
    fn plain_date_iso(vm: &Vm) -> ThrowCompletionOr<Value> {
        let temporal_time_zone_like = vm.argument(0);

        // 1. Let isoDateTime be ? SystemDateTime(temporalTimeZoneLike).
        let iso_date_time = system_date_time(vm, temporal_time_zone_like)?;

        // 2. Return ! CreateTemporalDate(isoDateTime.[[ISODate]], "iso8601").
        Ok(Value::from_object(
            create_temporal_date(
                vm,
                iso_date_time.iso_date,
                Utf16String::from_utf8(ISO8601_CALENDAR),
                None,
            )
            .must(),
        ))
    }

    // 2.2.6 Temporal.Now.plainTimeISO ( [ temporalTimeZoneLike ] ), https://tc39.es/proposal-temporal/#sec-temporal.now.plaintimeiso
    fn plain_time_iso(vm: &Vm) -> ThrowCompletionOr<Value> {
        let temporal_time_zone_like = vm.argument(0);

        // 1. Let isoDateTime be ? SystemDateTime(temporalTimeZoneLike).
        let iso_date_time = system_date_time(vm, temporal_time_zone_like)?;

        // 2. Return ! CreateTemporalTime(isoDateTime.[[Time]]).
        Ok(Value::from_object(
            create_temporal_time(vm, iso_date_time.time, None).must(),
        ))
    }
}

// 2.3.2 SystemUTCEpochMilliseconds ( ), https://tc39.es/proposal-temporal/#sec-temporal-systemutcepochmilliseconds
pub fn system_utc_epoch_milliseconds() -> f64 {
    // 1. Let global be GetGlobalObject().
    // 2. Let nowNs be HostSystemUTCEpochNanoseconds(global).
    // AD-HOC: Every caller of SystemUTCEpochMilliseconds is via Date.now and the Date constructor, which do not need
    //         nanosecond precision. We can avoid unnecessary bigint math by returning milliseconds directly.
    // NB: Like AK::UnixDateTime::now().nanoseconds_since_epoch(), this saturates to the i64 range.
    let since_epoch = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_or_else(
            |error| -(error.duration().as_nanos() as i128),
            |duration| duration.as_nanos() as i128,
        );
    let now_ns = since_epoch.clamp(i128::from(i64::MIN), i128::from(i64::MAX)) as i64;

    // 3. Return 𝔽(floor(nowNs / 10**6)).
    (now_ns / 1_000_000) as f64
}

// 2.3.3 SystemUTCEpochNanoseconds ( ), https://tc39.es/proposal-temporal/#sec-temporal-systemutcepochnanoseconds
pub fn system_utc_epoch_nanoseconds(vm: &Vm) -> SignedBigInteger {
    // 1. Let global be GetGlobalObject().
    let global = vm.get_global_object();

    // 2. Let nowNs be HostSystemUTCEpochNanoseconds(global).
    // 3. Return ℤ(nowNs).
    (vm.host_system_utc_epoch_nanoseconds())(vm, &global)
}

// 2.3.4 SystemDateTime ( temporalTimeZoneLike ), https://tc39.es/proposal-temporal/#sec-temporal-systemdatetime
pub fn system_date_time(vm: &Vm, temporal_time_zone_like: Value) -> ThrowCompletionOr<ISODateTime> {
    // 1. If temporalTimeZoneLike is undefined, then
    let time_zone = if temporal_time_zone_like.is_undefined() {
        // a. Let timeZone be SystemTimeZoneIdentifier().
        system_time_zone_identifier()
    }
    // 2. Else,
    else {
        // a. Let timeZone be ? ToTemporalTimeZoneIdentifier(temporalTimeZoneLike).
        to_temporal_time_zone_identifier(vm, temporal_time_zone_like)?
    };

    // 3. Let epochNs be SystemUTCEpochNanoseconds().
    let epoch_nanoseconds = system_utc_epoch_nanoseconds(vm);

    // 4. Return GetISODateTimeFor(timeZone, epochNs).
    Ok(get_iso_date_time_for(
        Utf16View::of_string(&time_zone),
        &epoch_nanoseconds,
    ))
}
