/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ops::Deref;

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::intl::intl_object::{IntlObject, ResolutionOptionDescriptor};
use crate::runtime::intl::mathematical_value::MathematicalValue;
use crate::runtime::intl::number_format::NumberFormatBase;
use crate::unicode::number_format::{self as unicode, PluralCategory, PluralForm};
use crate::utf16::Utf16View;

/// 17 PluralRules Objects, https://tc39.es/ecma402/#pluralrules-objects
#[repr(C)]
#[derive(Trace)]
pub struct PluralRules {
    base: NumberFormatBase,
    #[gc(untraced)]
    type_: Cell<PluralForm>, // [[Type]]
}

define_cell!(PluralRules, Object, extends: [NumberFormatBase, Object]);

impl Deref for PluralRules {
    type Target = NumberFormatBase;

    fn deref(&self) -> &NumberFormatBase {
        &self.base
    }
}

impl PluralRules {
    pub fn new(vm: &Vm, prototype: Gc<Object>) -> PluralRules {
        PluralRules {
            base: NumberFormatBase::new(vm, Self::CLASS, prototype),
            type_: Cell::new(PluralForm::Cardinal),
        }
    }

    pub fn type_(&self) -> PluralForm {
        self.type_.get()
    }

    pub fn type_string(&self) -> &'static str {
        unicode::plural_form_to_string(self.type_.get())
    }

    pub fn set_type(&self, type_: Utf16View<'_>) {
        self.type_.set(unicode::plural_form_from_string(type_));
    }
}

impl IntlObject for PluralRules {
    // 17.2.3 Internal slots, https://tc39.es/ecma402/#sec-intl.pluralrules-internal-slots
    fn relevant_extension_keys(&self) -> &'static [&'static str] {
        // The value of the [[RelevantExtensionKeys]] internal slot is « ».
        &[]
    }

    // 17.2.3 Internal slots, https://tc39.es/ecma402/#sec-intl.pluralrules-internal-slots
    fn resolution_option_descriptors<'vm>(&self, _: &'vm Vm) -> Vec<ResolutionOptionDescriptor<'vm>> {
        // The value of the [[ResolutionOptionDescriptors]] internal slot is « ».
        Vec::new()
    }
}

// 17.5.2 ResolvePlural ( pluralRules, n ), https://tc39.es/ecma402/#sec-resolveplural
pub fn resolve_plural(plural_rules: &PluralRules, number: &MathematicalValue) -> PluralCategory {
    // 1. If n is NOT-A-NUMBER, then
    if number.is_nan() {
        // a. Let s be an ILD String value indicating the NaN value.
        // b. Return the Record { [[PluralCategory]]: "other", [[FormattedString]]: s }.
        return PluralCategory::Other;
    }

    // 2. If n is POSITIVE-INFINITY, then
    if number.is_positive_infinity() {
        // a. Let s be an ILD String value indicating positive infinity.
        // b. Return the Record { [[PluralCategory]]: "other", [[FormattedString]]: s }.
        return PluralCategory::Other;
    }

    // 3. If n is NEGATIVE-INFINITY, then
    if number.is_negative_infinity() {
        // a. Let s be an ILD String value indicating negative infinity.
        // b. Return the Record { [[PluralCategory]]: "other", [[FormattedString]]: s }.
        return PluralCategory::Other;
    }

    // 4. Let res be FormatNumericToString(pluralRules, n).
    // 5. Let s be res.[[FormattedString]].
    // 6. Let locale be pluralRules.[[Locale]].
    // 7. Let type be pluralRules.[[Type]].
    // 8. Let notation be pluralRules.[[Notation]].
    // 9. Let compactDisplay be pluralRules.[[CompactDisplay]].
    // 10. Let p be PluralRuleSelect(locale, type, notation, compactDisplay, s).
    // 11. Return the Record { [[PluralCategory]]: p, [[FormattedString]]: s }.
    let value = number.to_value();
    plural_rules.with_formatter(|formatter| formatter.select_plural(&value))
}

// 17.5.4 ResolvePluralRange ( pluralRules, x, y ), https://tc39.es/ecma402/#sec-resolvepluralrange
pub fn resolve_plural_range(
    vm: &Vm,
    plural_rules: &PluralRules,
    start: &MathematicalValue,
    end: &MathematicalValue,
) -> ThrowCompletionOr<PluralCategory> {
    // 1. If x is NOT-A-NUMBER or y is NOT-A-NUMBER, throw a RangeError exception.
    if start.is_nan() {
        return vm.throw_completion(ErrorKind::RangeError, ErrorType::NumberIsNaN, &[&"start"]);
    }
    if end.is_nan() {
        return vm.throw_completion(ErrorKind::RangeError, ErrorType::NumberIsNaN, &[&"end"]);
    }

    // 2. Let xp be ResolvePlural(pluralRules, x).
    // 3. Let yp be ResolvePlural(pluralRules, y).
    // 4. If xp.[[FormattedString]] is yp.[[FormattedString]], then
    //     a. Return xp.[[PluralCategory]].
    // 5. Let locale be pluralRules.[[Locale]].
    // 6. Let type be pluralRules.[[Type]].
    // 7. Let notation be pluralRules.[[Notation]].
    // 8. Let compactDisplay be pluralRules.[[CompactDisplay]].
    // 9. Return PluralRuleSelectRange(locale, type, notation, compactDisplay, xp.[[PluralCategory]], yp.[[PluralCategory]]).
    let (start, end) = (start.to_value(), end.to_value());
    Ok(plural_rules.with_formatter(|formatter| formatter.select_plural_range(&start, &end)))
}
