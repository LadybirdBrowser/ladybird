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
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::intl::abstract_operations::{
    canonicalize_unicode_locale_id, is_well_formed_currency_code, is_well_formed_language_tag,
    throw_invalid_language_tag, to_ascii_lowercase, to_ascii_uppercase,
};
use crate::runtime::intl::intl_object::{IntlObject, ResolutionOptionDescriptor};
use crate::runtime::object::MayInterfereWithIndexedPropertyAccess;
use crate::runtime::primitive_string::PrimitiveString;
use crate::unicode::intl::{self as unicode, LanguageDisplay, Style};
use crate::utf16::Utf16View;

/// DisplayNames::Type.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Type {
    Invalid,
    Language,
    Region,
    Script,
    Currency,
    Calendar,
    DateTimeField,
}

/// DisplayNames::Fallback.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Fallback {
    Invalid,
    None,
    Code,
}

/// 12 DisplayNames Objects, https://tc39.es/ecma402/#intl-displaynames-objects
#[repr(C)]
#[derive(Trace)]
pub struct DisplayNames {
    base: Object,
    #[gc(untraced)]
    locale: GcRefCell<Utf16String>, // [[Locale]]
    #[gc(untraced)]
    style: Cell<Style>, // [[Style]]
    #[gc(untraced)]
    type_: Cell<Type>, // [[Type]]
    #[gc(untraced)]
    fallback: Cell<Fallback>, // [[Fallback]]
    #[gc(untraced)]
    language_display: Cell<Option<LanguageDisplay>>, // [[LanguageDisplay]]

    // Non-standard. Stores the ICU locale for display-name lookups.
    #[gc(untraced)]
    icu_locale: GcRefCell<Utf16String>,
}

define_cell!(DisplayNames, Object, extends: [Object]);

impl Deref for DisplayNames {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

impl DisplayNames {
    pub fn new(vm: &Vm, prototype: Gc<Object>) -> DisplayNames {
        DisplayNames {
            base: Object::new_with_prototype(vm, Self::CLASS, prototype, MayInterfereWithIndexedPropertyAccess::No),
            locale: GcRefCell::default(),
            style: Cell::new(Style::Long),
            type_: Cell::new(Type::Invalid),
            fallback: Cell::new(Fallback::Invalid),
            language_display: Cell::new(None),
            icu_locale: GcRefCell::default(),
        }
    }

    pub fn locale(&self) -> Utf16String {
        self.locale.borrow().clone()
    }

    pub fn set_locale(&self, locale: Utf16String) {
        self.locale.replace(locale);
    }

    pub fn icu_locale(&self) -> Utf16String {
        self.icu_locale.borrow().clone()
    }

    pub fn set_icu_locale(&self, icu_locale: Utf16String) {
        self.icu_locale.replace(icu_locale);
    }

    pub fn style(&self) -> Style {
        self.style.get()
    }

    pub fn set_style(&self, style: Utf16View<'_>) {
        self.style.set(unicode::style_from_string(style));
    }

    pub fn style_string(&self) -> &'static str {
        unicode::style_to_string(self.style.get())
    }

    pub fn type_(&self) -> Type {
        self.type_.get()
    }

    pub fn set_type(&self, type_: Utf16View<'_>) {
        self.type_.set(if type_ == "language" {
            Type::Language
        } else if type_ == "region" {
            Type::Region
        } else if type_ == "script" {
            Type::Script
        } else if type_ == "currency" {
            Type::Currency
        } else if type_ == "calendar" {
            Type::Calendar
        } else if type_ == "dateTimeField" {
            Type::DateTimeField
        } else {
            unreachable!("the type is one of the values GetOption allows")
        });
    }

    pub fn type_string(&self) -> &'static str {
        match self.type_.get() {
            Type::Language => "language",
            Type::Region => "region",
            Type::Script => "script",
            Type::Currency => "currency",
            Type::Calendar => "calendar",
            Type::DateTimeField => "dateTimeField",
            Type::Invalid => unreachable!("the Intl.DisplayNames constructor sets the type"),
        }
    }

    pub fn fallback(&self) -> Fallback {
        self.fallback.get()
    }

    pub fn set_fallback(&self, fallback: Utf16View<'_>) {
        self.fallback.set(if fallback == "none" {
            Fallback::None
        } else if fallback == "code" {
            Fallback::Code
        } else {
            unreachable!("the fallback is one of the values GetOption allows")
        });
    }

    pub fn fallback_string(&self) -> &'static str {
        match self.fallback.get() {
            Fallback::None => "none",
            Fallback::Code => "code",
            Fallback::Invalid => unreachable!("the Intl.DisplayNames constructor sets the fallback"),
        }
    }

    pub fn has_language_display(&self) -> bool {
        self.language_display.get().is_some()
    }

    pub fn language_display(&self) -> LanguageDisplay {
        self.language_display
            .get()
            .expect("the language display of an Intl.DisplayNames whose type is language")
    }

    pub fn set_language_display(&self, language_display: Utf16View<'_>) {
        self.language_display
            .set(Some(unicode::language_display_from_string(language_display)));
    }

    pub fn language_display_string(&self) -> &'static str {
        unicode::language_display_to_string(self.language_display())
    }
}

impl IntlObject for DisplayNames {
    // 12.2.3 Internal slots, https://tc39.es/ecma402/#sec-Intl.DisplayNames-internal-slots
    fn relevant_extension_keys(&self) -> &'static [&'static str] {
        // The value of the [[RelevantExtensionKeys]] internal slot is « ».
        &[]
    }

    // 12.2.3 Internal slots, https://tc39.es/ecma402/#sec-Intl.DisplayNames-internal-slots
    fn resolution_option_descriptors<'vm>(&self, _: &'vm Vm) -> Vec<ResolutionOptionDescriptor<'vm>> {
        // The value of the [[ResolutionOptionDescriptors]] internal slot is « ».
        Vec::new()
    }
}

fn invalid_code<T>(vm: &Vm, code: Utf16View<'_>, option: &str) -> ThrowCompletionOr<T> {
    vm.throw_completion_with_utf16_message(
        ErrorKind::RangeError,
        ErrorType::OptionIsNotValidValue.utf16_message(&[code, Utf16View::Ascii(option.as_bytes())]),
    )
}

fn string_value(vm: &Vm, string: Utf16String) -> Value {
    Value::from_string(PrimitiveString::create(vm, string))
}

// 12.5.1 CanonicalCodeForDisplayNames ( type, code ), https://tc39.es/ecma402/#sec-canonicalcodefordisplaynames
pub fn canonical_code_for_display_names(vm: &Vm, type_: Type, code: Utf16View<'_>) -> ThrowCompletionOr<Value> {
    // 1. If type is "language", then
    if type_ == Type::Language {
        // a. If code does not match the unicode_language_id production, throw a RangeError exception.
        if !unicode::is_unicode_language_id(code) {
            return invalid_code(vm, code, "language");
        }

        // b. If IsWellFormedLanguageTag(code) is false, throw a RangeError exception.
        if !is_well_formed_language_tag(code) {
            return throw_invalid_language_tag(vm, code);
        }

        // c. Return ! CanonicalizeUnicodeLocaleId(code).
        let canonicalized_tag = canonicalize_unicode_locale_id(vm, code)?;
        return Ok(string_value(vm, canonicalized_tag));
    }

    // 2. If type is "region", then
    if type_ == Type::Region {
        // a. If code does not match the unicode_region_subtag production, throw a RangeError exception.
        if !unicode::is_unicode_region_subtag(code) {
            return invalid_code(vm, code, "region");
        }

        // b. Return the ASCII-uppercase of code.
        return Ok(string_value(vm, to_ascii_uppercase(code)));
    }

    // 3. If type is "script", then
    if type_ == Type::Script {
        // a. If code does not match the unicode_script_subtag production, throw a RangeError exception.
        if !unicode::is_unicode_script_subtag(code) {
            return invalid_code(vm, code, "script");
        }

        // Assert: The length of code is 4, and every code unit of code represents an ASCII letter (0x0041 through 0x005A and 0x0061 through 0x007A, both inclusive).
        assert!(code.length_in_code_units() == 4);

        // c. Let first be the ASCII-uppercase of the substring of code from 0 to 1.
        // d. Let rest be the ASCII-lowercase of the substring of code from 1.
        // e. Return the string-concatenation of first and rest.
        let first = to_ascii_uppercase(code.substring_view(0, 1));
        let rest = to_ascii_lowercase(code.substring_view(1, 3));
        let mut builder = crate::utf16::Utf16StringBuilder::new();
        builder.append(Utf16View::of_string(&first));
        builder.append(Utf16View::of_string(&rest));
        return Ok(string_value(vm, builder.to_utf16_string()));
    }

    // 4. If type is "calendar", then
    if type_ == Type::Calendar {
        // a. If code does not match the Unicode Locale Identifier type nonterminal, throw a RangeError exception.
        if !unicode::is_type_identifier(code) {
            return invalid_code(vm, code, "calendar");
        }

        // b. If code uses any of the backwards compatibility syntax described in Unicode Technical Standard #35 LDML § 3.3 BCP 47 Conformance, throw a RangeError exception.
        if code.code_units().any(|code_unit| code_unit == u16::from(b'_')) {
            return invalid_code(vm, code, "calendar");
        }

        // c. Return the ASCII-lowercase of code.
        return Ok(string_value(vm, to_ascii_lowercase(code)));
    }

    // 5. If type is "dateTimeField", then
    if type_ == Type::DateTimeField {
        // a. If the result of IsValidDateTimeFieldCode(code) is false, throw a RangeError exception.
        if !is_valid_date_time_field_code(code) {
            return invalid_code(vm, code, "dateTimeField");
        }

        // b. Return code.
        return Ok(string_value(vm, code.to_utf16_string()));
    }

    // 6. Assert: type is "currency".
    assert!(type_ == Type::Currency);

    // 7. If ! IsWellFormedCurrencyCode(code) is false, throw a RangeError exception.
    if !is_well_formed_currency_code(code) {
        return invalid_code(vm, code, "currency");
    }

    // 8. Return the ASCII-uppercase of code.
    Ok(string_value(vm, to_ascii_uppercase(code)))
}

// 12.5.2 IsValidDateTimeFieldCode ( field ), https://tc39.es/ecma402/#sec-isvaliddatetimefieldcode
pub fn is_valid_date_time_field_code(field: Utf16View<'_>) -> bool {
    // 1. If field is listed in the Code column of Table 19, return true.
    // 2. Return false.
    [
        "era",
        "year",
        "quarter",
        "month",
        "weekOfYear",
        "weekday",
        "day",
        "dayPeriod",
        "hour",
        "minute",
        "second",
        "timeZoneName",
    ]
    .iter()
    .any(|code| field == *code)
}
