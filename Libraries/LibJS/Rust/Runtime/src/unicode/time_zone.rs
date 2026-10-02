/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The time zones of LibUnicode (Libraries/LibUnicode/TimeZone.h), through its C exports, so the runtime resolves
//! time zones and their offsets with the same ICU data as the C++ runtime.

use core::ffi::c_void;
use std::sync::OnceLock;

use ak::Utf16String;

use super::{UnicodeTextMappingOutput, collect_text};
use crate::utf16::Utf16View;

unsafe extern "C" {
    fn unicode_current_time_zone(output: UnicodeTextMappingOutput);
    fn unicode_set_current_time_zone(time_zone: *const u16, length: usize) -> bool;
    fn unicode_available_time_zones(
        context: *mut c_void,
        append: unsafe extern "C" fn(context: *mut c_void, time_zone: *const u16, length: usize),
    );
    fn unicode_resolve_primary_time_zone(
        time_zone: *const u16,
        length: usize,
        output: UnicodeTextMappingOutput,
    ) -> bool;
    fn unicode_time_zone_offset(
        time_zone: *const u16,
        length: usize,
        seconds: i64,
        nanoseconds: u32,
        offset_nanoseconds: *mut i64,
        in_dst: *mut bool,
    ) -> bool;
    fn unicode_disambiguated_time_zone_offsets(
        time_zone: *const u16,
        length: usize,
        seconds: i64,
        nanoseconds: u32,
        offset_nanoseconds: *mut i64,
        in_dst: *mut bool,
        capacity: usize,
    ) -> usize;
    fn unicode_time_zone_transition(
        time_zone: *const u16,
        length: usize,
        seconds: i64,
        nanoseconds: u32,
        direction: u8,
        include_given_time: bool,
        transition_rule: u8,
        transition_milliseconds: *mut i64,
    ) -> bool;
}

/// AK::UnixDateTime, as the whole seconds since the epoch and the nanoseconds within that second, the way the C
/// exports take it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct UnixDateTime {
    seconds: i64,
    nanoseconds: u32,
}

const NANOSECONDS_PER_SECOND: i64 = 1_000_000_000;

impl UnixDateTime {
    pub fn from_seconds_since_epoch(seconds: i64) -> Self {
        Self {
            seconds,
            nanoseconds: 0,
        }
    }

    pub fn from_milliseconds_since_epoch(milliseconds: i64) -> Self {
        Self {
            seconds: milliseconds.div_euclid(1_000),
            nanoseconds: u32::try_from(milliseconds.rem_euclid(1_000) * 1_000_000)
                .expect("the nanoseconds within a second fit in a u32"),
        }
    }

    pub fn from_nanoseconds_since_epoch(nanoseconds: i64) -> Self {
        Self {
            seconds: nanoseconds.div_euclid(NANOSECONDS_PER_SECOND),
            nanoseconds: u32::try_from(nanoseconds.rem_euclid(NANOSECONDS_PER_SECOND))
                .expect("the nanoseconds within a second fit in a u32"),
        }
    }
}

/// Unicode::TimeZoneOffset::InDST.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum InDST {
    No,
    Yes,
}

impl InDST {
    fn from_bool(in_dst: bool) -> Self {
        if in_dst { Self::Yes } else { Self::No }
    }
}

/// Unicode::TimeZoneOffset, whose AK::Duration offset is kept as the nanoseconds it amounts to.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TimeZoneOffset {
    pub offset_nanoseconds: i64,
    pub in_dst: InDST,
}

/// Unicode::TimeZoneTransition::Options::Direction.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum TransitionDirection {
    Previous,
    Next,
}

/// Unicode::TimeZoneTransition::Options::IncludeGivenTime.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum IncludeGivenTime {
    No,
    Yes,
}

/// Unicode::TimeZoneTransition::Options::TransitionRule.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum TransitionRule {
    AnyTransition,
    TransitionWhereUTCOffsetChanges,
}

/// Unicode::TimeZoneTransition::Options.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TimeZoneTransitionOptions {
    pub direction: TransitionDirection,
    pub include_given_time: IncludeGivenTime,
    pub transition_rule: TransitionRule,
}

fn code_units_of(time_zone: Utf16View<'_>) -> Vec<u16> {
    time_zone.code_units().collect()
}

/// Unicode::current_time_zone: the host's time zone, which LibUnicode caches.
pub fn current_time_zone() -> Utf16String {
    // SAFETY: The output writes into a Vec that outlives the call.
    let ((), time_zone) = collect_text(|output| unsafe { unicode_current_time_zone(output) });
    Utf16String::from_utf16(&time_zone)
}

/// Unicode::set_current_time_zone: makes `time_zone` ICU's default time zone and the one current_time_zone() returns.
/// Returns false if LibUnicode does not know the time zone.
pub fn set_current_time_zone(time_zone: Utf16View<'_>) -> bool {
    let time_zone = code_units_of(time_zone);
    // SAFETY: The time zone buffer is valid for its length.
    unsafe { unicode_set_current_time_zone(time_zone.as_ptr(), time_zone.len()) }
}

unsafe extern "C" fn append_time_zone(context: *mut c_void, time_zone: *const u16, length: usize) {
    // SAFETY: The context is the list available_time_zones() passes, which outlives the call.
    let time_zones = unsafe { &mut *context.cast::<Vec<Vec<u16>>>() };
    // SAFETY: LibUnicode passes a time zone of `length` code units that stays alive for the call.
    time_zones.push(unsafe { core::slice::from_raw_parts(time_zone, length) }.to_vec());
}

/// Unicode::available_time_zones: every time zone LibUnicode knows, sorted, which it computes once.
pub fn available_time_zones() -> &'static [Vec<u16>] {
    static TIME_ZONES: OnceLock<Vec<Vec<u16>>> = OnceLock::new();
    TIME_ZONES.get_or_init(|| {
        let mut time_zones: Vec<Vec<u16>> = Vec::new();
        // SAFETY: The callback appends to the list passed as its context, which outlives the call.
        unsafe { unicode_available_time_zones((&raw mut time_zones).cast(), append_time_zone) };
        time_zones
    })
}

/// Unicode::resolve_primary_time_zone: the IANA zone that `time_zone` resolves to.
pub fn resolve_primary_time_zone(time_zone: Utf16View<'_>) -> Option<Utf16String> {
    let time_zone = code_units_of(time_zone);
    let (resolved, primary) = collect_text(|output| {
        // SAFETY: The time zone buffer is valid for its length, and the output writes into a Vec.
        unsafe { unicode_resolve_primary_time_zone(time_zone.as_ptr(), time_zone.len(), output) }
    });
    resolved.then(|| Utf16String::from_utf16(&primary))
}

/// Unicode::time_zone_offset: the offset from UTC of `time_zone` at `time`.
pub fn time_zone_offset(time_zone: Utf16View<'_>, time: UnixDateTime) -> Option<TimeZoneOffset> {
    let time_zone = code_units_of(time_zone);
    let mut offset_nanoseconds = 0;
    let mut in_dst = false;
    // SAFETY: The time zone buffer is valid for its length, and the offset and DST flag are written into locals.
    let found = unsafe {
        unicode_time_zone_offset(
            time_zone.as_ptr(),
            time_zone.len(),
            time.seconds,
            time.nanoseconds,
            &raw mut offset_nanoseconds,
            &raw mut in_dst,
        )
    };
    found.then(|| TimeZoneOffset {
        offset_nanoseconds,
        in_dst: InDST::from_bool(in_dst),
    })
}

/// Unicode::disambiguated_time_zone_offsets: the offsets `time_zone` may have at the local time `time`, which is
/// none in a gap, two in an overlap, the earlier transition's offset first, and one otherwise.
pub fn disambiguated_time_zone_offsets(time_zone: Utf16View<'_>, time: UnixDateTime) -> Vec<TimeZoneOffset> {
    const CAPACITY: usize = 2;

    let time_zone = code_units_of(time_zone);
    let mut offset_nanoseconds = [0_i64; CAPACITY];
    let mut in_dst = [false; CAPACITY];
    // SAFETY: The time zone buffer is valid for its length, and the offsets and DST flags are written into arrays of
    //         CAPACITY elements, which LibUnicode verifies it does not exceed.
    let count = unsafe {
        unicode_disambiguated_time_zone_offsets(
            time_zone.as_ptr(),
            time_zone.len(),
            time.seconds,
            time.nanoseconds,
            offset_nanoseconds.as_mut_ptr(),
            in_dst.as_mut_ptr(),
            CAPACITY,
        )
    };
    (0..count)
        .map(|index| TimeZoneOffset {
            offset_nanoseconds: offset_nanoseconds[index],
            in_dst: InDST::from_bool(in_dst[index]),
        })
        .collect()
}

/// Unicode::get_time_zone_transition: the milliseconds since the epoch of the transition of `time_zone` from `time`
/// that `options` asks for.
pub fn get_time_zone_transition(
    time_zone: Utf16View<'_>,
    time: UnixDateTime,
    options: TimeZoneTransitionOptions,
) -> Option<i64> {
    let time_zone = code_units_of(time_zone);
    let mut transition_milliseconds = 0;
    // SAFETY: The time zone buffer is valid for its length, and the transition is written into a local.
    let found = unsafe {
        unicode_time_zone_transition(
            time_zone.as_ptr(),
            time_zone.len(),
            time.seconds,
            time.nanoseconds,
            options.direction as u8,
            options.include_given_time == IncludeGivenTime::Yes,
            options.transition_rule as u8,
            &raw mut transition_milliseconds,
        )
    };
    found.then_some(transition_milliseconds)
}
