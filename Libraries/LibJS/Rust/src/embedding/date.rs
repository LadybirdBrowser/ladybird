/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Creating Date objects from time values and reading the time values back, and the time value arithmetic of
//! ECMA-262's Date section, which the embedder shares with the runtime instead of computing it twice.
//!
//! The functions here follow the contract object.rs states for the embedding module. A Date crosses as its JSObject,
//! and the functions that take one abort for any other object, as the C++ as<JS::Date>() does. The arithmetic is pure
//! and may run on any thread.

#![allow(
    clippy::missing_safety_doc,
    reason = "object.rs states the contract every exported function shares"
)]

use crate::embedding::abi_types::{JSRealm, cell_from_abi, object_into_abi, vm_from_abi};
use crate::layout::host_class::{JSObject, JSVM};
use crate::runtime::date::{
    Date, date_from_time, hour_from_time, make_date, make_day, make_time, min_from_time, month_from_time, ms_from_time,
    sec_from_time, year_from_time,
};

/// Date::create(realm, date_value): a Date of the realm's %Date.prototype% whose [[DateValue]] is the time value,
/// which is not clipped. Returns an unrooted date. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_date_create(vm: *mut JSVM, realm: *mut JSRealm, date_value: f64) -> *mut JSObject {
    // SAFETY: See the module documentation.
    let (vm, realm) = unsafe { (vm_from_abi(vm), cell_from_abi::<JSRealm>(realm)) };
    object_into_abi(Date::create(vm, realm, date_value))
}

/// [[DateValue]]: the time value of the date, NaN for an invalid one. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_date_date_value(date: *mut JSObject) -> f64 {
    // SAFETY: See the module documentation.
    unsafe { cell_from_abi::<JSObject>(date) }
        .downcast::<Date>()
        .expect("the object is a Date")
        .date_value()
}

/// YearFromTime ( t ).
#[unsafe(no_mangle)]
pub extern "C" fn js_date_year_from_time(time: f64) -> i32 {
    year_from_time(time)
}

/// MonthFromTime ( t ), 0 for January.
#[unsafe(no_mangle)]
pub extern "C" fn js_date_month_from_time(time: f64) -> u8 {
    month_from_time(time)
}

/// DateFromTime ( t ), the day of the month from 1.
#[unsafe(no_mangle)]
pub extern "C" fn js_date_date_from_time(time: f64) -> u8 {
    date_from_time(time)
}

/// HourFromTime ( t ).
#[unsafe(no_mangle)]
pub extern "C" fn js_date_hour_from_time(time: f64) -> u8 {
    hour_from_time(time)
}

/// MinFromTime ( t ).
#[unsafe(no_mangle)]
pub extern "C" fn js_date_min_from_time(time: f64) -> u8 {
    min_from_time(time)
}

/// SecFromTime ( t ).
#[unsafe(no_mangle)]
pub extern "C" fn js_date_sec_from_time(time: f64) -> u8 {
    sec_from_time(time)
}

/// msFromTime ( t ).
#[unsafe(no_mangle)]
pub extern "C" fn js_date_ms_from_time(time: f64) -> u16 {
    ms_from_time(time)
}

/// MakeTime ( hour, min, sec, ms ).
#[unsafe(no_mangle)]
pub extern "C" fn js_date_make_time(hour: f64, min: f64, sec: f64, ms: f64) -> f64 {
    make_time(hour, min, sec, ms)
}

/// MakeDay ( year, month, date ), with month 0 for January.
#[unsafe(no_mangle)]
pub extern "C" fn js_date_make_day(year: f64, month: f64, date: f64) -> f64 {
    make_day(year, month, date)
}

/// MakeDate ( day, time ).
#[unsafe(no_mangle)]
pub extern "C" fn js_date_make_date(day: f64, time: f64) -> f64 {
    make_date(day, time)
}
