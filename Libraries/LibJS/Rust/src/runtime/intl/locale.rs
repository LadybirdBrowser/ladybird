/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ops::Deref;

use ak::Utf16String;
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::gc::gc_ref_cell::GcRefCell;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::array::Array;
use crate::runtime::intl::abstract_operations::create_array_from_string_list;
use crate::runtime::object::MayInterfereWithIndexedPropertyAccess;
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::realm::Realm;
use crate::unicode::intl::{self as unicode, LocaleID, Weekday};
use crate::utf16::Utf16View;

/// 15 Locale Objects, https://tc39.es/ecma402/#locale-objects
#[repr(C)]
#[derive(Trace)]
pub struct Locale {
    base: Object,
    #[gc(untraced)]
    locale: GcRefCell<Utf16String>, // [[Locale]]
    #[gc(untraced)]
    calendar: GcRefCell<Option<Utf16String>>, // [[Calendar]]
    #[gc(untraced)]
    case_first: GcRefCell<Option<Utf16String>>, // [[CaseFirst]]
    #[gc(untraced)]
    collation: GcRefCell<Option<Utf16String>>, // [[Collation]]
    #[gc(untraced)]
    first_day_of_week: GcRefCell<Option<Utf16String>>, // [[FirstDayOfWeek]]
    #[gc(untraced)]
    hour_cycle: GcRefCell<Option<Utf16String>>, // [[HourCycle]]
    #[gc(untraced)]
    numbering_system: GcRefCell<Option<Utf16String>>, // [[NumberingSystem]]
    #[gc(untraced)]
    numeric: Cell<bool>, // [[Numeric]]
    #[gc(untraced)]
    cached_locale_id: GcRefCell<Option<LocaleID>>,
}

define_cell!(Locale, Object, extends: [Object]);

impl Deref for Locale {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

impl Locale {
    pub fn new(vm: &Vm, prototype: Gc<Object>) -> Locale {
        Locale {
            base: Object::new_with_prototype(vm, Self::CLASS, prototype, MayInterfereWithIndexedPropertyAccess::No),
            locale: GcRefCell::default(),
            calendar: GcRefCell::default(),
            case_first: GcRefCell::default(),
            collation: GcRefCell::default(),
            first_day_of_week: GcRefCell::default(),
            hour_cycle: GcRefCell::default(),
            numbering_system: GcRefCell::default(),
            numeric: Cell::new(false),
            cached_locale_id: GcRefCell::default(),
        }
    }

    pub fn create(vm: &Vm, realm: Gc<Realm>, source_locale: Gc<Locale>, locale_tag: Utf16String) -> Gc<Locale> {
        let locale = realm.create_object(vm, Locale::new(vm, realm.intrinsics().intl_locale_prototype(vm)));

        locale.set_locale(locale_tag);
        locale.calendar.replace(source_locale.calendar());
        locale.case_first.replace(source_locale.case_first());
        locale.collation.replace(source_locale.collation());
        locale.hour_cycle.replace(source_locale.hour_cycle());
        locale.numbering_system.replace(source_locale.numbering_system());
        locale.numeric.set(source_locale.numeric());

        locale
    }

    /// The parsed [[Locale]], which is parsed the first time it is asked for.
    pub fn locale_id(&self) -> LocaleID {
        if let Some(locale_id) = self.cached_locale_id.borrow().as_ref() {
            return locale_id.clone();
        }
        let locale_id = unicode::parse_unicode_locale_id(Utf16View::of_string(&self.locale()))
            .expect("the [[Locale]] of an Intl.Locale is a Unicode locale identifier");
        self.cached_locale_id.replace(Some(locale_id.clone()));
        locale_id
    }

    pub fn locale(&self) -> Utf16String {
        self.locale.borrow().clone()
    }

    pub fn set_locale(&self, locale: Utf16String) {
        self.locale.replace(locale);
    }

    pub fn calendar(&self) -> Option<Utf16String> {
        self.calendar.borrow().clone()
    }

    pub fn set_calendar(&self, calendar: Utf16String) {
        self.calendar.replace(Some(calendar));
    }

    pub fn case_first(&self) -> Option<Utf16String> {
        self.case_first.borrow().clone()
    }

    pub fn set_case_first(&self, case_first: Utf16String) {
        self.case_first.replace(Some(case_first));
    }

    pub fn collation(&self) -> Option<Utf16String> {
        self.collation.borrow().clone()
    }

    pub fn set_collation(&self, collation: Utf16String) {
        self.collation.replace(Some(collation));
    }

    pub fn first_day_of_week(&self) -> Option<Utf16String> {
        self.first_day_of_week.borrow().clone()
    }

    pub fn set_first_day_of_week(&self, first_day_of_week: Utf16String) {
        self.first_day_of_week.replace(Some(first_day_of_week));
    }

    pub fn hour_cycle(&self) -> Option<Utf16String> {
        self.hour_cycle.borrow().clone()
    }

    pub fn set_hour_cycle(&self, hour_cycle: Utf16String) {
        self.hour_cycle.replace(Some(hour_cycle));
    }

    pub fn numbering_system(&self) -> Option<Utf16String> {
        self.numbering_system.borrow().clone()
    }

    pub fn set_numbering_system(&self, numbering_system: Utf16String) {
        self.numbering_system.replace(Some(numbering_system));
    }

    pub fn numeric(&self) -> bool {
        self.numeric.get()
    }

    pub fn set_numeric(&self, numeric: bool) {
        self.numeric.set(numeric);
    }

    /// 15.2.2 Internal slots, https://tc39.es/ecma402/#sec-intl.locale-internal-slots
    pub const fn locale_extension_keys() -> &'static [&'static str] {
        // The value of the [[LocaleExtensionKeys]] internal slot is a List that must include all elements of
        // « "ca", "co", "fw", "hc", "nu" », must additionally include any element of « "kf", "kn" » that is also an
        // element of %Intl.Collator%.[[RelevantExtensionKeys]], and must not include any other elements.
        &["ca", "co", "fw", "hc", "kf", "kn", "nu"]
    }
}

/// Whether `value` is an object with an [[InitializedLocale]] internal slot.
pub fn as_locale(value: Value) -> Option<Gc<Locale>> {
    if !value.is_object() {
        return None;
    }
    value.as_object().downcast::<Locale>()
}

/// Table 27: WeekInfo Record Fields, https://tc39.es/ecma402/#sec-weekinfooflocale
pub struct WeekInfo {
    pub first_day: u8,    // [[FirstDay]]
    pub weekend: Vec<u8>, // [[Weekend]]
}

// 15.5.5 GetLocaleVariants ( locale ), https://tc39.es/ecma402/#sec-getlocalevariants
pub fn get_locale_variants(locale: &LocaleID) -> Option<Utf16String> {
    // 1. Let baseName be GetLocaleBaseName(locale).
    let base_name = &locale.language_id;

    // 2. NOTE: Each subtag in baseName that is preceded by "-" is either a unicode_script_subtag, unicode_region_subtag,
    //    or unicode_variant_subtag, but any substring matched by unicode_variant_subtag is strictly longer than any
    //    prefix thereof which could also be matched by one of the other productions.

    // 3. Let variants be the longest suffix of baseName that starts with a "-" followed by a substring that is matched
    //    by the unicode_variant_subtag Unicode locale nonterminal. If there is no such suffix, return undefined.
    if base_name.variants.is_empty() {
        return None;
    }

    // 4. Return the substring of variants from 1.
    let views: Vec<Utf16View<'_>> = base_name.variants.iter().map(Utf16View::of_string).collect();
    Some(join_with_hyphens(&views))
}

/// Utf16String::join("-"sv, strings).
pub fn join_with_hyphens(strings: &[Utf16View<'_>]) -> Utf16String {
    let mut builder = crate::utf16::Utf16StringBuilder::new();
    for (index, string) in strings.iter().enumerate() {
        if index > 0 {
            builder.append_ascii("-");
        }
        builder.append(*string);
    }
    builder.to_utf16_string()
}

fn create_array_from_single_string(vm: &Vm, realm: Gc<Realm>, string: Utf16String) -> Gc<Array> {
    let value = Value::from_string(PrimitiveString::create(vm, string));
    Array::create_from(vm, realm, &[value])
}

// 15.5.9 CalendarsOfLocale ( loc ), https://tc39.es/ecma402/#sec-calendarsoflocale
pub fn calendars_of_locale(vm: &Vm, locale_object: &Locale) -> Gc<Array> {
    let realm = vm.current_realm().expect("CalendarsOfLocale runs in a realm");

    // 1. If loc.[[Calendar]] is not undefined, then
    if let Some(calendar) = locale_object.calendar() {
        // a. Return CreateArrayFromList(« loc.[[Calendar]] »).
        return create_array_from_single_string(vm, realm, calendar);
    }

    // 2. Let preference be RegionPreference(loc.[[Locale]]).
    // 3. Let region be preference.[[Region]].
    // 4. Let regionOverride be preference.[[RegionOverride]].
    // 5. If regionOverride is not undefined and calendar preference data for regionOverride are available, then
    //     a. Let lookupRegion be regionOverride.
    // 6. Else,
    //     a. Let lookupRegion be region.
    // 7. Let list be a List of unique calendar types in canonical form (6.9), sorted in descending preference of those
    //    in common use for date and time formatting in lookupRegion. The list is empty if no calendar preference data
    //    for lookupRegion is available.
    // 8. If list is empty, set list to « "gregory" ».
    let list = unicode::available_calendars_of_locale(Utf16View::of_string(&locale_object.locale()));

    // 9. Return CreateArrayFromList(list).
    create_array_from_string_list(vm, realm, &list)
}

// 15.5.10 CollationsOfLocale ( loc ), https://tc39.es/ecma402/#sec-collationsoflocale
pub fn collations_of_locale(vm: &Vm, locale_object: &Locale) -> Gc<Array> {
    let realm = vm.current_realm().expect("CollationsOfLocale runs in a realm");

    // 1. If loc.[[Collation]] is not undefined, then
    if let Some(collation) = locale_object.collation() {
        // a. Return CreateArrayFromList(« loc.[[Collation]] »).
        return create_array_from_single_string(vm, realm, collation);
    }

    // 2. Let language be GetLocaleLanguage(loc.[[Locale]]).
    // 3. If language is not "und", then
    //     a. Let r be LookupMatchingLocaleByPrefix(%Intl.Collator%.[[AvailableLocales]], « loc.[[Locale]] »).
    //     b. If r is not undefined, then
    //         i. Let foundLocale be r.[[locale]].
    //     c. Else,
    //         i. Let foundLocale be DefaultLocale().
    //     d. Let foundLocaleData be %Intl.Collator%.[[SortLocaleData]].[[<foundLocale>]].
    //     e. Let list be a copy of foundLocaleData.[[co]].
    //     f. Assert: list[0] is null.
    //     g. Remove the first element from list.
    // 4. Else,
    //     a. Let list be « "emoji", "eor" ».
    // 5. Let sorted be a copy of list, sorted according to lexicographic code unit order.
    let list = unicode::available_collations_of_locale(Utf16View::of_string(&locale_object.locale()));

    // 6. Return CreateArrayFromList(sorted).
    create_array_from_string_list(vm, realm, &list)
}

// 15.5.11 HourCyclesOfLocale ( loc ), https://tc39.es/ecma402/#sec-hourcyclesoflocale
pub fn hour_cycles_of_locale(vm: &Vm, locale_object: &Locale) -> Gc<Array> {
    let realm = vm.current_realm().expect("HourCyclesOfLocale runs in a realm");

    // 1. If loc.[[HourCycle]] is not undefined, then
    if let Some(hour_cycle) = locale_object.hour_cycle() {
        // a. Return CreateArrayFromList(« loc.[[HourCycle]] »).
        return create_array_from_single_string(vm, realm, hour_cycle);
    }

    // 2. Let preference be RegionPreference(loc.[[Locale]]).
    // 3. Let region be preference.[[Region]].
    // 4. Let regionOverride be preference.[[RegionOverride]].
    // 5. If regionOverride is not undefined and time data for regionOverride are available, then
    //     a. Let lookupRegion be regionOverride.
    // 6. Else,
    //     a. Let lookupRegion be region.
    // 7. Let list be a List of unique hour cycle identifiers, which must be lower case String values indicating either the 12-hour format ("h11", "h12") or the 24-hour format ("h23", "h24"), sorted in descending preference of those in common use for date and time formatting in lookupRegion. The list is empty if no time data for lookupRegion is available.
    // 8. If list is empty, set list to « "h23" ».
    let list = unicode::available_hour_cycles_of_locale(Utf16View::of_string(&locale_object.locale()));

    // 9. Return CreateArrayFromList(list).
    create_array_from_string_list(vm, realm, &list)
}

// 15.5.12 NumberingSystemsOfLocale ( loc ), https://tc39.es/ecma402/#sec-numberingsystemsoflocale
pub fn numbering_systems_of_locale(vm: &Vm, locale_object: &Locale) -> Gc<Array> {
    let realm = vm.current_realm().expect("NumberingSystemsOfLocale runs in a realm");

    // 1. If loc.[[NumberingSystem]] is not undefined, then
    if let Some(numbering_system) = locale_object.numbering_system() {
        // a. Return CreateArrayFromList(« loc.[[NumberingSystem]] »).
        return create_array_from_single_string(vm, realm, numbering_system);
    }

    // 2. Let r be LookupMatchingLocaleByPrefix(%Intl.NumberFormat%.[[AvailableLocales]], « loc.[[Locale]] »).
    // 3. If r is not undefined, then
    //     a. Let foundLocale be r.[[locale]].
    //     b. Let foundLocaleData be %Intl.NumberFormat%.[[LocaleData]].[[<foundLocale>]].
    //     c. Let numberingSystems be foundLocaleData.[[nu]].
    //     d. Let list be « numberingSystems[0] ».
    // 4. Else,
    //     a. Let list be « "latn" ».
    let list = unicode::available_number_systems_of_locale(Utf16View::of_string(&locale_object.locale()));

    // 5. Return CreateArrayFromList(list).
    create_array_from_string_list(vm, realm, &list)
}

// 15.5.13 TimeZonesOfLocale ( loc ), https://tc39.es/ecma402/#sec-timezonesoflocale
pub fn time_zones_of_locale(vm: &Vm, locale_object: &Locale) -> Value {
    let realm = vm.current_realm().expect("TimeZonesOfLocale runs in a realm");

    // 1. Let region be GetLocaleRegion(loc.[[Locale]]).
    let locale_id = locale_object.locale_id();

    // 2. If region is undefined, return undefined.
    let Some(region) = &locale_id.language_id.region else {
        return Value::UNDEFINED;
    };

    // 3. Let list be a List of unique canonical time zone identifiers, which must be String values indicating a
    //    canonical Zone name of the IANA Time Zone Database, of those in common use in region. The list is empty if no
    //    time zones are commonly used in region. The list is sorted according to lexicographic code unit order.
    let list = unicode::available_time_zones_in_region(Utf16View::of_string(region));

    // 4. Return CreateArrayFromList( list ).
    Value::from_object(create_array_from_string_list(vm, realm, &list))
}

// 15.5.14 TextDirectionOfLocale ( loc ), https://tc39.es/ecma402/#sec-textdirectionoflocale
pub fn text_direction_of_locale(locale_object: &Locale) -> &'static str {
    // 1. Let locale be loc.[[Locale]].
    let locale = locale_object.locale();

    // 2. Let script be GetLocaleScript(locale).
    // 3. If script is undefined, then
    //     a. Let maximal be the result of the Add Likely Subtags algorithm applied to locale. If an error is signaled, return undefined.
    //     b. Set script to GetLocaleScript(maximal).
    //     c. If script is undefined, return undefined.
    // NB: ICU handles maximizing the locale if there is no script.

    // 4. If the default general ordering of characters within a line in script is right-to-left, return "rtl".
    // 5. If the default general ordering of characters within a line in script is left-to-right, return "ltr".
    // 6. Return undefined.
    // FIXME: ICU does not provide a method to determine if a locale is neither rtl nor ltr.
    if unicode::is_locale_character_ordering_right_to_left(Utf16View::of_string(&locale)) {
        "rtl"
    } else {
        "ltr"
    }
}

struct FirstDayStringAndValue {
    weekday: &'static str,
    string: &'static str,
    value: u8,
}

// Table 26: Weekday String and Value, https://tc39.es/ecma402/#table-locale-weekday-string-value
const WEEKDAY_STRING_AND_VALUE: [FirstDayStringAndValue; 8] = [
    FirstDayStringAndValue {
        weekday: "0",
        string: "sun",
        value: 7,
    },
    FirstDayStringAndValue {
        weekday: "1",
        string: "mon",
        value: 1,
    },
    FirstDayStringAndValue {
        weekday: "2",
        string: "tue",
        value: 2,
    },
    FirstDayStringAndValue {
        weekday: "3",
        string: "wed",
        value: 3,
    },
    FirstDayStringAndValue {
        weekday: "4",
        string: "thu",
        value: 4,
    },
    FirstDayStringAndValue {
        weekday: "5",
        string: "fri",
        value: 5,
    },
    FirstDayStringAndValue {
        weekday: "6",
        string: "sat",
        value: 6,
    },
    FirstDayStringAndValue {
        weekday: "7",
        string: "sun",
        value: 7,
    },
];

// 15.5.15 WeekdayToUValue ( fw ), https://tc39.es/ecma402/#sec-weekdaytouvalue
pub fn weekday_to_u_value(weekday: Utf16View<'_>) -> Utf16String {
    // 1. For each row of Table 26, except the header row, in table order, do
    for row in &WEEKDAY_STRING_AND_VALUE {
        // a. Let w be the Weekday value of the current row.
        // b. Let s be the String value of the current row.
        // c. If fw is equal to w, return s.
        if weekday == row.weekday {
            return Utf16String::from_utf8(row.string);
        }
    }

    // 2. Return fw.
    weekday.to_utf16_string()
}

// 15.5.16 WeekdayUValueToNumber ( fw ), https://tc39.es/ecma402/#sec-weekdayuvaluetonumber
pub fn weekday_u_value_to_number(weekday: Utf16View<'_>) -> Option<u8> {
    // 1. For each row of Table 26, except the header row, in table order, do
    for row in &WEEKDAY_STRING_AND_VALUE {
        // a. Let s be the String value of the current row.
        // b. Let v be the Value value of the current row.
        // c. If fw is equal to s, return v.
        if weekday == row.string {
            return Some(row.value);
        }
    }

    // 2. Return undefined.
    None
}

fn weekday_to_integer(weekday: Option<Weekday>, fallback: Weekday) -> u8 {
    // NB: This fallback will be used if the ICU data lookup failed. Its value should be that of the default region
    //     ("001") in the CLDR.
    match weekday.unwrap_or(fallback) {
        Weekday::Monday => 1,
        Weekday::Tuesday => 2,
        Weekday::Wednesday => 3,
        Weekday::Thursday => 4,
        Weekday::Friday => 5,
        Weekday::Saturday => 6,
        Weekday::Sunday => 7,
    }
}

fn weekend_of_locale(weekend_days: &[Weekday]) -> Vec<u8> {
    let mut weekend: Vec<u8> = weekend_days
        .iter()
        .map(|&day| weekday_to_integer(Some(day), day))
        .collect();
    weekend.sort_unstable();
    weekend
}

// 15.5.17 WeekInfoOfLocale ( loc ), https://tc39.es/ecma402/#sec-weekinfooflocale
pub fn week_info_of_locale(locale_object: &Locale) -> WeekInfo {
    // 1. Let locale be loc.[[Locale]].
    let locale = locale_object.locale();

    // 2. Let r be a Record whose fields are defined by Table 27, with values based on locale.
    let locale_week_info = unicode::week_info_of_locale(Utf16View::of_string(&locale));

    let mut week_info = WeekInfo {
        first_day: weekday_to_integer(locale_week_info.first_day_of_week, Weekday::Monday),
        weekend: weekend_of_locale(&locale_week_info.weekend_days),
    };

    let mut first_day_of_week = None;

    if let Some(first_day_of_week_string) = locale_object.first_day_of_week() {
        // 3. Let fws be loc.[[FirstDayOfWeek]].
        // 4. Let fw be WeekdayUValueToNumber(fws).
        first_day_of_week = weekday_u_value_to_number(Utf16View::of_string(&first_day_of_week_string));
    }

    // 5. If fw is not undefined, then
    if let Some(first_day_of_week) = first_day_of_week {
        // a. Set r.[[FirstDay]] to fw.
        week_info.first_day = first_day_of_week;
    }

    // 6. Return r.
    week_info
}
