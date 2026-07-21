/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The CSS calculation tree.
//!
//! https://www.w3.org/TR/css-values-4/#calculation-tree
//!
//! This is the beginning of the calc-tree port: the Rust data model that will
//! be built alongside the C++ CalculationNode tree, mirroring its node kinds.
//! Nothing constructs these yet; the FFI construction surface follows with
//! the C++ staging.

use std::sync::Arc;

use crate::style_value::RetainedStyleValue;

include!(concat!(env!("OUT_DIR"), "/dimension_units_generated.rs"));

/// The numeric leaf of a calculation: a raw value in one of the numeric
/// dimensions, mirroring CalculationResult::Value. Units cross as the same
/// opaque codes the style value data uses.
#[derive(Clone, Copy, PartialEq)]
pub enum CalcNumericValue {
    /// The number type mirrors the C++ Number::Type discriminants
    /// (number, integer with explicit sign, integer), pinned C++-side.
    Number {
        value: f64,
        number_type: u8,
    },
    Angle {
        value: f64,
        unit: u8,
    },
    Flex {
        value: f64,
        unit: u8,
    },
    Frequency {
        value: f64,
        unit: u8,
    },
    Length {
        value: f64,
        unit: u8,
    },
    Percentage(f64),
    Resolution {
        value: f64,
        unit: u8,
    },
    Time {
        value: f64,
        unit: u8,
    },
}

pub(crate) const BASE_TYPE_COUNT: usize = 7;
pub(crate) const BASE_TYPE_PERCENT: usize = 6;

/// https://drafts.css-houdini.org/css-typed-om-1/#numeric-typing
/// The type of a calculation: an optional exponent per base type plus the
/// percent hint, mirroring the C++ NumericType. Absent and zero exponents are
/// distinct, exactly as in the typed-om algebra.
#[derive(Clone, Copy, PartialEq, Eq, Default, Debug)]
pub struct CalcNumericType {
    /// Exponents indexed by base type: length, angle, time, frequency,
    /// resolution, flex, percent, in the C++ BaseType order.
    pub exponents: [Option<i32>; BASE_TYPE_COUNT],
    /// The percent hint: an index into the base types, when set.
    pub percent_hint: Option<u8>,
}

impl CalcNumericType {
    fn contains_all_the_non_zero_entries_of_other_with_the_same_value(&self, other: &CalcNumericType) -> bool {
        for i in 0..BASE_TYPE_COUNT {
            let other_exponent = other.exponents[i];
            if other_exponent.is_some() && other_exponent != Some(0) && self.exponents[i] != other_exponent {
                return false;
            }
        }
        true
    }

    fn contains_a_key_other_than_percent_with_a_non_zero_value(&self) -> bool {
        (0..BASE_TYPE_COUNT)
            .any(|i| i != BASE_TYPE_PERCENT && self.exponents[i].is_some() && self.exponents[i] != Some(0))
    }

    /// Copies entries from `other`, optionally skipping the ones already present.
    fn copy_all_entries_from(&mut self, other: &CalcNumericType, skip_if_already_present: bool) {
        for i in 0..BASE_TYPE_COUNT {
            if other.exponents[i].is_some() && !(skip_if_already_present && self.exponents[i].is_some()) {
                self.exponents[i] = other.exponents[i];
            }
        }
    }

    /// https://drafts.css-houdini.org/css-typed-om-1/#apply-the-percent-hint
    fn apply_percent_hint(&mut self, hint: u8) {
        // To apply the percent hint hint to a type without a percent hint, perform the following steps:
        assert!(self.percent_hint.is_none());
        let hint_index = hint as usize;

        // 1. Set type's percent hint to hint.
        self.percent_hint = Some(hint);

        // 2. If type doesn't contain hint, set type[hint] to 0.
        if self.exponents[hint_index].is_none() {
            self.exponents[hint_index] = Some(0);
        }

        // 3. If hint is anything other than "percent", and type contains "percent",
        //    add type["percent"] to type[hint], then set type["percent"] to 0.
        if hint_index != BASE_TYPE_PERCENT && self.exponents[BASE_TYPE_PERCENT].is_some() {
            self.exponents[hint_index] =
                Some(self.exponents[BASE_TYPE_PERCENT].unwrap() + self.exponents[hint_index].unwrap());
            self.exponents[BASE_TYPE_PERCENT] = Some(0);
        }
    }

    /// https://drafts.css-houdini.org/css-typed-om-1/#cssnumericvalue-add-two-types
    pub(crate) fn added_to(&self, other: &CalcNumericType) -> Option<CalcNumericType> {
        // To add two types type1 and type2, perform the following steps:

        // 1. Replace type1 with a fresh copy of type1, and type2 with a fresh copy of type2.
        //    Let finalType be a new type with an initially empty ordered map and an initially null percent hint.
        let mut type1 = *self;
        let mut type2 = *other;
        let mut final_type = CalcNumericType::default();

        // 2. If both type1 and type2 have non-null percent hints with different values
        if type1.percent_hint.is_some() && type2.percent_hint.is_some() && type1.percent_hint != type2.percent_hint {
            // The types can't be added. Return failure.
            return None;
        }
        //    If type1 has a non-null percent hint hint and type2 doesn't
        if let (Some(hint), None) = (type1.percent_hint, type2.percent_hint) {
            // Apply the percent hint hint to type2.
            type2.apply_percent_hint(hint);
        }
        //    Vice versa if type2 has a non-null percent hint and type1 doesn't.
        else if let (Some(hint), None) = (type2.percent_hint, type1.percent_hint) {
            type1.apply_percent_hint(hint);
        }

        // 3. If all the entries of type1 with non-zero values are contained in type2 with the same value, and vice-versa
        if type2.contains_all_the_non_zero_entries_of_other_with_the_same_value(&type1)
            && type1.contains_all_the_non_zero_entries_of_other_with_the_same_value(&type2)
        {
            // Copy all of type1's entries to finalType, and then copy all of type2's entries to finalType that
            // finalType doesn't already contain. Set finalType's percent hint to type1's percent hint. Return finalType.
            final_type.copy_all_entries_from(&type1, false);
            final_type.copy_all_entries_from(&type2, true);
            final_type.percent_hint = type1.percent_hint;
            return Some(final_type);
        }
        //    If type1 and/or type2 contain "percent" with a non-zero value,
        //    and type1 and/or type2 contain a key other than "percent" with a non-zero value
        let percent_is_non_zero =
            |t: &CalcNumericType| t.exponents[BASE_TYPE_PERCENT].is_some() && t.exponents[BASE_TYPE_PERCENT] != Some(0);
        if (percent_is_non_zero(&type1) || percent_is_non_zero(&type2))
            && (type1.contains_a_key_other_than_percent_with_a_non_zero_value()
                || type2.contains_a_key_other_than_percent_with_a_non_zero_value())
        {
            // For each base type other than "percent" hint:
            for hint in 0..BASE_TYPE_COUNT as u8 {
                if hint as usize == BASE_TYPE_PERCENT {
                    continue;
                }

                // 1. Provisionally apply the percent hint hint to both type1 and type2.
                let mut provisional_type1 = type1;
                provisional_type1.apply_percent_hint(hint);
                let mut provisional_type2 = type2;
                provisional_type2.apply_percent_hint(hint);

                // 2. If, afterwards, all the entries of type1 with non-zero values are contained in type2
                //    with the same value, and vice versa, then copy all of type1's entries to finalType,
                //    and then copy all of type2's entries to finalType that finalType doesn't already contain.
                //    Set finalType's percent hint to hint. Return finalType.
                if provisional_type2.contains_all_the_non_zero_entries_of_other_with_the_same_value(&provisional_type1)
                    && provisional_type1
                        .contains_all_the_non_zero_entries_of_other_with_the_same_value(&provisional_type2)
                {
                    final_type.copy_all_entries_from(&provisional_type1, false);
                    final_type.copy_all_entries_from(&provisional_type2, true);
                    final_type.percent_hint = Some(hint);
                    return Some(final_type);
                }

                // 3. Otherwise, revert type1 and type2 to their state at the start of this loop.
                // NOTE: The modifications were made to the provisional copies, so this is a no-op.
            }

            // If the loop finishes without returning finalType, then the types can't be added. Return failure.
            return None;
        }
        // Otherwise
        //     The types can't be added. Return failure.
        None
    }

    /// https://drafts.css-houdini.org/css-typed-om-1/#cssnumericvalue-multiply-two-types
    pub(crate) fn multiplied_by(&self, other: &CalcNumericType) -> Option<CalcNumericType> {
        // To multiply two types type1 and type2, perform the following steps:

        // 1. Replace type1 with a fresh copy of type1, and type2 with a fresh copy of type2.
        //    Let finalType be a new type with an initially empty ordered map and an initially null percent hint.
        let mut type1 = *self;
        let mut type2 = *other;
        let mut final_type = CalcNumericType::default();

        // 2. If both type1 and type2 have non-null percent hints with different values,
        //    the types can't be multiplied. Return failure.
        if type1.percent_hint.is_some() && type2.percent_hint.is_some() && type1.percent_hint != type2.percent_hint {
            return None;
        }

        // 3. If type1 has a non-null percent hint hint and type2 doesn't, apply the percent hint hint to type2.
        if let (Some(hint), None) = (type1.percent_hint, type2.percent_hint) {
            type2.apply_percent_hint(hint);
        }
        //    Vice versa if type2 has a non-null percent hint and type1 doesn't.
        else if let (Some(hint), None) = (type2.percent_hint, type1.percent_hint) {
            type1.apply_percent_hint(hint);
        }

        // 4. Copy all of type1's entries to finalType, then for each baseType -> power of type2:
        final_type.copy_all_entries_from(&type1, false);
        for i in 0..BASE_TYPE_COUNT {
            let Some(power) = type2.exponents[i] else {
                continue;
            };
            // 1. If finalType[baseType] exists, increment its value by power.
            // 2. Otherwise, set finalType[baseType] to power.
            final_type.exponents[i] = Some(final_type.exponents[i].unwrap_or(0) + power);
        }
        //    Set finalType's percent hint to type1's percent hint.
        final_type.percent_hint = type1.percent_hint;

        // 5. Return finalType.
        Some(final_type)
    }

    /// https://drafts.css-houdini.org/css-typed-om-1/#cssnumericvalue-invert-a-type
    pub(crate) fn inverted(&self) -> CalcNumericType {
        // To invert a type type, perform the following steps:

        // 1. Let result be a new type with an initially empty ordered map and a percent hint matching that of type.
        let mut result = CalcNumericType {
            percent_hint: self.percent_hint,
            ..Default::default()
        };

        // 2. For each unit -> exponent of type, set result[unit] to (-1 * exponent).
        for i in 0..BASE_TYPE_COUNT {
            if let Some(power) = self.exponents[i] {
                result.exponents[i] = Some(-power);
            }
        }

        // 3. Return result.
        result
    }

    /// https://drafts.csswg.org/css-values-4/#css-make-a-type-consistent
    pub(crate) fn made_consistent_with(&self, input: &CalcNumericType) -> Option<CalcNumericType> {
        let mut base = *self;

        // 1. If both base and input have different non-null percent hints, they can't be made consistent. Return failure.
        if base.percent_hint.is_some() && input.percent_hint.is_some() && base.percent_hint != input.percent_hint {
            return None;
        }

        // 2. If base has a null percent hint set base's percent hint to input's percent hint.
        if base.percent_hint.is_none() {
            base.percent_hint = input.percent_hint;
        }

        // 3. Return base.
        Some(base)
    }
}

/// The result of evaluating a calculation: the numeric value in canonical
/// units and the numeric type it carries.
#[derive(Clone, Copy, PartialEq, Debug)]
pub(crate) struct CalcResult {
    pub value: f64,
    pub numeric_type: Option<CalcNumericType>,
}

impl CalcResult {
    pub(crate) fn multiply_by(&mut self, other: &CalcResult) {
        self.value *= other.value;
        self.numeric_type = match (&self.numeric_type, &other.numeric_type) {
            (Some(first), Some(second)) => first.multiplied_by(second),
            _ => None,
        };
    }

    pub(crate) fn invert(&mut self) {
        // FIXME: Correctly handle division by zero.
        self.value = 1.0 / self.value;
        if let Some(numeric_type) = &self.numeric_type {
            self.numeric_type = Some(numeric_type.inverted());
        }
    }
}

/// Resolves a length not handled by the shared resolver (container-relative
/// units, which need the per-element query container lookup), to pixels.
pub(crate) type LengthFallbackResolver<'a> = dyn Fn(f64, u8) -> Option<f64> + 'a;

/// The length resolution information available in an evaluation: the shared
/// context plus an optional per-element fallback for units it cannot handle.
#[derive(Clone, Copy, Default)]
pub(crate) struct LengthResolution<'a> {
    pub context: Option<&'a crate::style_compute::FfiLengthResolutionContext>,
    pub fallback: Option<&'a LengthFallbackResolver<'a>>,
}

impl CalcNumericValue {
    /// The value in its dimension's canonical unit: degrees, fr, hertz,
    /// unrounded pixels, dots per pixel, or seconds. Lengths resolve through
    /// the length resolution context; a relative length without one is NaN,
    /// as in the C++ CalculationResult::from_value.
    pub(crate) fn to_canonical_number(self, length_resolution: LengthResolution) -> f64 {
        match self {
            CalcNumericValue::Number { value, .. } => value,
            CalcNumericValue::Percentage(value) => value,
            CalcNumericValue::Angle { value, unit } => value * ANGLE_UNIT_CANONICAL_RATIOS[unit as usize],
            CalcNumericValue::Flex { value, unit } => value * FLEX_UNIT_CANONICAL_RATIOS[unit as usize],
            CalcNumericValue::Frequency { value, unit } => value * FREQUENCY_UNIT_CANONICAL_RATIOS[unit as usize],
            CalcNumericValue::Resolution { value, unit } => value * RESOLUTION_UNIT_CANONICAL_RATIOS[unit as usize],
            CalcNumericValue::Time { value, unit } => value * TIME_UNIT_CANONICAL_RATIOS[unit as usize],
            CalcNumericValue::Length { value, unit } => {
                // Absolute lengths resolve without a context.
                let ratio = crate::style_compute::LENGTH_UNIT_CANONICAL_PX_RATIOS[unit as usize];
                if ratio.is_finite() {
                    return value * ratio;
                }
                let Some(context) = length_resolution.context else {
                    return f64::NAN;
                };
                let result = crate::style_compute::absolutize_length_for_calc(value, unit as usize, context);
                if result.handled {
                    return result.px;
                }
                length_resolution
                    .fallback
                    .and_then(|fallback| fallback(value, unit))
                    .unwrap_or(f64::NAN)
            }
        }
    }
}

/// The FFI mirror of a numeric type, for the parity test on the C++ side.
/// NB: The array dimension is the base type count, spelled literally so the
///     generated header does not depend on the crate-private constant.
#[repr(C)]
pub struct FfiNumericType {
    pub has_exponent: [bool; 7],
    pub exponents: [i32; 7],
    pub has_percent_hint: bool,
    pub percent_hint: u8,
    pub valid: bool,
}

impl FfiNumericType {
    fn from_calc(value: Option<CalcNumericType>) -> Self {
        let mut result = FfiNumericType {
            has_exponent: [false; BASE_TYPE_COUNT],
            exponents: [0; BASE_TYPE_COUNT],
            has_percent_hint: false,
            percent_hint: 0,
            valid: value.is_some(),
        };
        if let Some(value) = value {
            for i in 0..BASE_TYPE_COUNT {
                if let Some(exponent) = value.exponents[i] {
                    result.has_exponent[i] = true;
                    result.exponents[i] = exponent;
                }
            }
            if let Some(hint) = value.percent_hint {
                result.has_percent_hint = true;
                result.percent_hint = hint;
            }
        }
        result
    }

    fn to_calc(&self) -> CalcNumericType {
        let mut result = CalcNumericType::default();
        for i in 0..BASE_TYPE_COUNT {
            if self.has_exponent[i] {
                result.exponents[i] = Some(self.exponents[i]);
            }
        }
        if self.has_percent_hint {
            result.percent_hint = Some(self.percent_hint);
        }
        result
    }
}

/// FFI parity hooks for the C++ parity test: operation 0 adds, 1 multiplies,
/// 2 inverts (always valid), 3 makes consistent.
///
/// # Safety
/// Both pointers must be valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_numeric_type_operate(
    operation: u8,
    first: *const FfiNumericType,
    second: *const FfiNumericType,
) -> FfiNumericType {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CalcOperationEntry);
    crate::abort_on_panic(|| {
        let first = unsafe { &*first }.to_calc();
        let second = unsafe { &*second }.to_calc();
        let result = match operation {
            0 => first.added_to(&second),
            1 => first.multiplied_by(&second),
            2 => Some(first.inverted()),
            3 => first.made_consistent_with(&second),
            _ => unreachable!("invalid numeric type operation {operation}"),
        };
        FfiNumericType::from_calc(result)
    })
}

/// https://drafts.csswg.org/css-values-4/#round-func
/// The rounding strategy of round(), opaque code from the C++ side.
pub type CalcRoundingStrategy = u8;

/// One node of a calculation tree. Child nodes are shared immutably.
///
/// https://www.w3.org/TR/css-values-4/#calculation-tree
pub enum CalcNode {
    /// A numeric leaf value.
    Numeric(CalcNumericValue),
    /// https://drafts.csswg.org/css-color-5/#relative-color
    /// A color channel keyword inside a relative color, opaque code.
    ChannelKeyword(u8),
    Sum(Vec<Arc<CalcNode>>),
    Product(Vec<Arc<CalcNode>>),
    Negate(Arc<CalcNode>),
    Invert(Arc<CalcNode>),
    Min(Vec<Arc<CalcNode>>),
    Max(Vec<Arc<CalcNode>>),
    Clamp {
        min: Arc<CalcNode>,
        center: Arc<CalcNode>,
        max: Arc<CalcNode>,
    },
    /// https://drafts.csswg.org/css-values-5/#progress-func
    Progress {
        no_clamp: bool,
        progress: Arc<CalcNode>,
        from: Arc<CalcNode>,
        to: Arc<CalcNode>,
    },
    Abs(Arc<CalcNode>),
    Sign(Arc<CalcNode>),
    Sin(Arc<CalcNode>),
    Cos(Arc<CalcNode>),
    Tan(Arc<CalcNode>),
    Asin(Arc<CalcNode>),
    Acos(Arc<CalcNode>),
    Atan(Arc<CalcNode>),
    Atan2 {
        y: Arc<CalcNode>,
        x: Arc<CalcNode>,
    },
    Pow {
        base: Arc<CalcNode>,
        exponent: Arc<CalcNode>,
    },
    Sqrt(Arc<CalcNode>),
    Hypot(Vec<Arc<CalcNode>>),
    Log {
        value: Arc<CalcNode>,
        base: Arc<CalcNode>,
    },
    Exp(Arc<CalcNode>),
    Round {
        strategy: CalcRoundingStrategy,
        value: Arc<CalcNode>,
        interval: Arc<CalcNode>,
    },
    Mod {
        value: Arc<CalcNode>,
        modulus: Arc<CalcNode>,
    },
    Rem {
        value: Arc<CalcNode>,
        divisor: Arc<CalcNode>,
    },
    /// https://drafts.csswg.org/css-values-5/#random
    Random {
        min: Arc<CalcNode>,
        max: Arc<CalcNode>,
        step: Option<Arc<CalcNode>>,
        /// The random-value-sharing options value, retained from the shell.
        sharing: RetainedStyleValue,
    },
    /// A non-math function whose value participates in a calculation, kept as
    /// its retained style value together with the numeric type its context
    /// determined at creation.
    NonMathFunction {
        value: RetainedStyleValue,
        numeric_type: CalcNumericType,
    },
}

impl CalcNode {
    /// The node's children, for the traversals that do not care about the
    /// node kind.
    pub(crate) fn for_each_child(&self, f: &mut impl FnMut(&Arc<CalcNode>)) {
        match self {
            CalcNode::Numeric(..) | CalcNode::ChannelKeyword(..) | CalcNode::NonMathFunction { .. } => {}
            CalcNode::Sum(children)
            | CalcNode::Product(children)
            | CalcNode::Min(children)
            | CalcNode::Max(children)
            | CalcNode::Hypot(children) => children.iter().for_each(f),
            CalcNode::Negate(child)
            | CalcNode::Invert(child)
            | CalcNode::Abs(child)
            | CalcNode::Sign(child)
            | CalcNode::Sin(child)
            | CalcNode::Cos(child)
            | CalcNode::Tan(child)
            | CalcNode::Asin(child)
            | CalcNode::Acos(child)
            | CalcNode::Atan(child)
            | CalcNode::Sqrt(child)
            | CalcNode::Exp(child) => f(child),
            CalcNode::Clamp { min, center, max } => {
                f(min);
                f(center);
                f(max);
            }
            CalcNode::Progress { progress, from, to, .. } => {
                f(progress);
                f(from);
                f(to);
            }
            CalcNode::Atan2 { y, x } => {
                f(y);
                f(x);
            }
            CalcNode::Pow { base, exponent } => {
                f(base);
                f(exponent);
            }
            CalcNode::Log { value, base } => {
                f(value);
                f(base);
            }
            CalcNode::Round { value, interval, .. } => {
                f(value);
                f(interval);
            }
            CalcNode::Mod { value, modulus } => {
                f(value);
                f(modulus);
            }
            CalcNode::Rem { value, divisor } => {
                f(value);
                f(divisor);
            }
            CalcNode::Random { min, max, step, .. } => {
                f(min);
                f(max);
                if let Some(step) = step {
                    f(step);
                }
            }
        }
    }

    /// https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    /// The type of the calculation, mirroring the types the C++ nodes compute
    /// at creation. `percentage_leaf_type` is the type a percentage leaf takes
    /// in the surrounding context.
    pub(crate) fn numeric_type(&self, percentage_leaf_type: &CalcNumericType) -> Option<CalcNumericType> {
        let single = |base: usize| {
            let mut result = CalcNumericType::default();
            result.exponents[base] = Some(1);
            Some(result)
        };
        let add_children = |children: &[&Arc<CalcNode>]| {
            let mut left: Option<CalcNumericType> = None;
            for child in children {
                let right = child.numeric_type(percentage_leaf_type)?;
                left = Some(match left {
                    Some(left) => left.added_to(&right)?,
                    None => right,
                });
            }
            left
        };
        match self {
            // Anything else is a terminal value, whose type is determined based on its CSS type.
            CalcNode::Numeric(value) => match value {
                // -> <number> / <integer>: the type is «[ ]» (empty map)
                CalcNumericValue::Number { .. } => Some(CalcNumericType::default()),
                // -> each dimension: «[ dimension -> 1 ]», in the base type order.
                CalcNumericValue::Length { .. } => single(0),
                CalcNumericValue::Angle { .. } => single(1),
                CalcNumericValue::Time { .. } => single(2),
                CalcNumericValue::Frequency { .. } => single(3),
                CalcNumericValue::Resolution { .. } => single(4),
                CalcNumericValue::Flex { .. } => single(5),
                // -> <percentage>: the context-determined type.
                CalcNumericValue::Percentage(..) => Some(*percentage_leaf_type),
            },
            CalcNode::ChannelKeyword(..) => Some(CalcNumericType::default()),
            CalcNode::Sum(children) | CalcNode::Min(children) | CalcNode::Max(children) | CalcNode::Hypot(children) => {
                add_children(&children.iter().collect::<Vec<_>>())
            }
            CalcNode::Product(children) => {
                let mut left: Option<CalcNumericType> = None;
                for child in children {
                    let right = child.numeric_type(percentage_leaf_type)?;
                    left = Some(match left {
                        Some(left) => left.multiplied_by(&right)?,
                        None => right,
                    });
                }
                left
            }
            // NOTE: `- foo` doesn't change the type, and neither does abs().
            CalcNode::Negate(child) | CalcNode::Abs(child) => child.numeric_type(percentage_leaf_type),
            CalcNode::Invert(child) => Some(child.numeric_type(percentage_leaf_type)?.inverted()),
            CalcNode::Clamp { min, center, max } => add_children(&[min, center, max]),
            CalcNode::Progress { progress, from, to, .. } => {
                let sum = add_children(&[progress, from, to])?;
                CalcNumericType::default().made_consistent_with(&sum)
            }
            CalcNode::Round { value, interval, .. } => add_children(&[value, interval]),
            CalcNode::Mod { value, modulus } => add_children(&[value, modulus]),
            CalcNode::Rem { value, divisor } => add_children(&[value, divisor]),
            CalcNode::Random { min, max, step, .. } => match step {
                Some(step) => add_children(&[min, max, step]),
                None => add_children(&[min, max]),
            },
            // «[ ]» (empty map).
            CalcNode::Sign(..)
            | CalcNode::Sin(..)
            | CalcNode::Cos(..)
            | CalcNode::Tan(..)
            | CalcNode::Pow { .. }
            | CalcNode::Sqrt(..)
            | CalcNode::Log { .. }
            | CalcNode::Exp(..) => Some(CalcNumericType::default()),
            // «[ "angle" -> 1 ]».
            CalcNode::Asin(..) | CalcNode::Acos(..) | CalcNode::Atan(..) | CalcNode::Atan2 { .. } => single(1),
            CalcNode::NonMathFunction { numeric_type, .. } => Some(*numeric_type),
        }
    }

    /// https://drafts.css-houdini.org/css-properties-values-api/#computationally-independent
    /// Whether the calculation is computationally independent: every node is a
    /// conjunction over its children, with length leaves depending on their
    /// unit's relativity and the style values carried by random() and
    /// non-math-function nodes resolved through the given leaf resolver.
    pub(crate) fn is_computationally_independent(
        &self,
        length_is_independent: &impl Fn(u8) -> bool,
        style_value_is_independent: &impl Fn(&RetainedStyleValue) -> bool,
    ) -> bool {
        let leaf_independent = match self {
            CalcNode::Numeric(CalcNumericValue::Length { unit, .. }) => length_is_independent(*unit),
            CalcNode::Numeric(..) | CalcNode::ChannelKeyword(..) => true,
            CalcNode::Random { sharing, .. } => style_value_is_independent(sharing),
            CalcNode::NonMathFunction { value, .. } => style_value_is_independent(value),
            _ => true,
        };
        if !leaf_independent {
            return false;
        }
        let mut independent = true;
        self.for_each_child(&mut |child| {
            independent =
                independent && child.is_computationally_independent(length_is_independent, style_value_is_independent);
        });
        independent
    }

    /// https://www.w3.org/TR/css-values-4/#calculation-tree
    /// Whether any leaf of the subtree is a percentage.
    pub(crate) fn contains_percentage(&self) -> bool {
        if matches!(self, CalcNode::Numeric(CalcNumericValue::Percentage(..))) {
            return true;
        }
        let mut found = false;
        self.for_each_child(&mut |child| {
            found = found || child.contains_percentage();
        });
        found
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn contains_percentage_walks_the_tree() {
        let percent = Arc::new(CalcNode::Numeric(CalcNumericValue::Percentage(50.0)));
        let number = Arc::new(CalcNode::Numeric(CalcNumericValue::Number {
            value: 2.0,
            number_type: 0,
        }));
        let sum = CalcNode::Sum(vec![number.clone(), percent]);
        assert!(sum.contains_percentage());
        let product = CalcNode::Product(vec![number.clone(), number]);
        assert!(!product.contains_percentage());
    }

    #[test]
    fn numeric_type_defaults_to_empty() {
        let numeric_type = CalcNumericType::default();
        assert_eq!(numeric_type.exponents, [None; BASE_TYPE_COUNT]);
        assert!(numeric_type.percent_hint.is_none());
    }

    #[test]
    fn adding_percentage_to_length_hints_the_percent() {
        // «[ "length" -> 1 ]» + «[ "percent" -> 1 ]» resolves with a length percent hint.
        let mut length = CalcNumericType::default();
        length.exponents[0] = Some(1);
        let mut percent = CalcNumericType::default();
        percent.exponents[BASE_TYPE_PERCENT] = Some(1);
        let sum = length.added_to(&percent).expect("length + percent is consistent");
        assert_eq!(sum.percent_hint, Some(0));
        assert_eq!(sum.exponents[0], Some(1));
        // Incompatible dimensions fail.
        let mut angle = CalcNumericType::default();
        angle.exponents[1] = Some(1);
        assert!(length.added_to(&angle).is_none());
    }

    #[test]
    fn multiplying_types_adds_exponents() {
        let mut length = CalcNumericType::default();
        length.exponents[0] = Some(1);
        let product = length.multiplied_by(&length).expect("length * length");
        assert_eq!(product.exponents[0], Some(2));
        assert_eq!(length.inverted().exponents[0], Some(-1));
    }
}

/// A Rust-owned calculation node handle: an `Arc<CalcNode>` as a raw pointer,
/// repr(C) since it is embedded by value in the style value data. Ownership of
/// one strong count transfers with the handle.
#[repr(C)]
pub struct CalcNodeHandle {
    node: *const CalcNode,
}

impl CalcNodeHandle {
    /// # Safety
    /// `raw` must be a handle from one of the construction functions below.
    pub(crate) unsafe fn from_raw(raw: *const CalcNode) -> Self {
        Self { node: raw }
    }

    pub(crate) fn node(&self) -> &CalcNode {
        unsafe { &*self.node }
    }

    /// Borrows the handle's node as a shared reference without consuming the
    /// handle's strong count; the clone bumps and the temporary drops it.
    pub(crate) fn node_arc(&self) -> Arc<CalcNode> {
        unsafe { Arc::increment_strong_count(self.node) };
        unsafe { Arc::from_raw(self.node) }
    }
}

impl Drop for CalcNodeHandle {
    fn drop(&mut self) {
        drop(unsafe { Arc::from_raw(self.node) });
    }
}

// NB: Calculation trees hold retained C++ style value references, which are
//     main-thread-only until the shells become atomically refcounted, the same
//     contract the style group payload callbacks follow; the trees are only
//     built and dropped on the main thread today.
#[allow(clippy::arc_with_non_send_sync)]
fn handle(node: CalcNode) -> *const CalcNode {
    Arc::into_raw(Arc::new(node))
}

// NB: Same main-thread-only contract as `handle` above.
#[allow(clippy::arc_with_non_send_sync)]
fn shared(node: CalcNode) -> Arc<CalcNode> {
    Arc::new(node)
}

/// Reconstructs the child Arcs from an array of transferred handles.
///
/// # Safety
/// `children` must point at `count` valid transferred handles.
unsafe fn children_from_raw(children: *const *const CalcNode, count: usize) -> Vec<Arc<CalcNode>> {
    (0..count).map(|i| unsafe { Arc::from_raw(*children.add(i)) }).collect()
}

/// Creates a numeric leaf for a dimension: kind selects the dimension in the
/// order number, angle, flex, frequency, length, percentage, resolution, time.
#[unsafe(no_mangle)]
pub extern "C" fn rust_calc_node_create_numeric_dimension(kind: u8, value: f64, unit: u8) -> *const CalcNode {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CalcNodeBuildEntry);
    crate::abort_on_panic(|| {
        let numeric = match kind {
            0 => CalcNumericValue::Number {
                value,
                number_type: unit,
            },
            1 => CalcNumericValue::Angle { value, unit },
            2 => CalcNumericValue::Flex { value, unit },
            3 => CalcNumericValue::Frequency { value, unit },
            4 => CalcNumericValue::Length { value, unit },
            5 => CalcNumericValue::Percentage(value),
            6 => CalcNumericValue::Resolution { value, unit },
            7 => CalcNumericValue::Time { value, unit },
            _ => unreachable!("invalid numeric dimension kind {kind}"),
        };
        handle(CalcNode::Numeric(numeric))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_calc_node_create_channel_keyword(channel: u8) -> *const CalcNode {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CalcNodeBuildEntry);
    crate::abort_on_panic(|| handle(CalcNode::ChannelKeyword(channel)))
}

/// Creates a variadic node: kind selects sum (0), product (1), min (2),
/// max (3) or hypot (4).
///
/// # Safety
/// `children` must point at `count` valid transferred handles.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_calc_node_create_variadic(
    kind: u8,
    children: *const *const CalcNode,
    count: usize,
) -> *const CalcNode {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CalcNodeBuildEntry);
    crate::abort_on_panic(|| {
        let children = unsafe { children_from_raw(children, count) };
        let node = match kind {
            0 => CalcNode::Sum(children),
            1 => CalcNode::Product(children),
            2 => CalcNode::Min(children),
            3 => CalcNode::Max(children),
            4 => CalcNode::Hypot(children),
            _ => unreachable!("invalid variadic calc node kind {kind}"),
        };
        handle(node)
    })
}

/// Creates a single-child node: kind selects negate (0), invert (1), abs (2),
/// sign (3), sin (4), cos (5), tan (6), asin (7), acos (8), atan (9),
/// sqrt (10) or exp (11).
///
/// # Safety
/// `child` must be a valid transferred handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_calc_node_create_unary(kind: u8, child: *const CalcNode) -> *const CalcNode {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CalcNodeBuildEntry);
    crate::abort_on_panic(|| {
        let child = unsafe { Arc::from_raw(child) };
        let node = match kind {
            0 => CalcNode::Negate(child),
            1 => CalcNode::Invert(child),
            2 => CalcNode::Abs(child),
            3 => CalcNode::Sign(child),
            4 => CalcNode::Sin(child),
            5 => CalcNode::Cos(child),
            6 => CalcNode::Tan(child),
            7 => CalcNode::Asin(child),
            8 => CalcNode::Acos(child),
            9 => CalcNode::Atan(child),
            10 => CalcNode::Sqrt(child),
            11 => CalcNode::Exp(child),
            _ => unreachable!("invalid unary calc node kind {kind}"),
        };
        handle(node)
    })
}

/// Creates a two-child node: kind selects atan2 (0), pow (1), log (2),
/// mod (3) or rem (4), with the children in the C++ member order.
///
/// # Safety
/// Both children must be valid transferred handles.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_calc_node_create_binary(
    kind: u8,
    first: *const CalcNode,
    second: *const CalcNode,
) -> *const CalcNode {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CalcNodeBuildEntry);
    crate::abort_on_panic(|| {
        let first = unsafe { Arc::from_raw(first) };
        let second = unsafe { Arc::from_raw(second) };
        let node = match kind {
            0 => CalcNode::Atan2 { y: first, x: second },
            1 => CalcNode::Pow {
                base: first,
                exponent: second,
            },
            2 => CalcNode::Log {
                value: first,
                base: second,
            },
            3 => CalcNode::Mod {
                value: first,
                modulus: second,
            },
            4 => CalcNode::Rem {
                value: first,
                divisor: second,
            },
            _ => unreachable!("invalid binary calc node kind {kind}"),
        };
        handle(node)
    })
}

/// # Safety
/// All three children must be valid transferred handles.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_calc_node_create_clamp(
    min: *const CalcNode,
    center: *const CalcNode,
    max: *const CalcNode,
) -> *const CalcNode {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CalcNodeBuildEntry);
    crate::abort_on_panic(|| {
        handle(CalcNode::Clamp {
            min: unsafe { Arc::from_raw(min) },
            center: unsafe { Arc::from_raw(center) },
            max: unsafe { Arc::from_raw(max) },
        })
    })
}

/// # Safety
/// All three children must be valid transferred handles.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_calc_node_create_progress(
    no_clamp: bool,
    progress: *const CalcNode,
    from: *const CalcNode,
    to: *const CalcNode,
) -> *const CalcNode {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CalcNodeBuildEntry);
    crate::abort_on_panic(|| {
        handle(CalcNode::Progress {
            no_clamp,
            progress: unsafe { Arc::from_raw(progress) },
            from: unsafe { Arc::from_raw(from) },
            to: unsafe { Arc::from_raw(to) },
        })
    })
}

/// # Safety
/// Both children must be valid transferred handles.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_calc_node_create_round(
    strategy: u8,
    value: *const CalcNode,
    interval: *const CalcNode,
) -> *const CalcNode {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CalcNodeBuildEntry);
    crate::abort_on_panic(|| {
        handle(CalcNode::Round {
            strategy,
            value: unsafe { Arc::from_raw(value) },
            interval: unsafe { Arc::from_raw(interval) },
        })
    })
}

/// # Safety
/// The children must be valid transferred handles (`step` may be null), and
/// `sharing` a leaked strong StyleValue reference.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_calc_node_create_random(
    min: *const CalcNode,
    max: *const CalcNode,
    step: *const CalcNode,
    sharing: *const std::ffi::c_void,
) -> *const CalcNode {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CalcNodeBuildEntry);
    crate::abort_on_panic(|| {
        handle(CalcNode::Random {
            min: unsafe { Arc::from_raw(min) },
            max: unsafe { Arc::from_raw(max) },
            step: if step.is_null() {
                None
            } else {
                Some(unsafe { Arc::from_raw(step) })
            },
            sharing: unsafe { RetainedStyleValue::from_shell_pointer(sharing) },
        })
    })
}

/// # Safety
/// `value` must be a leaked strong StyleValue reference.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_calc_node_create_non_math_function(
    value: *const std::ffi::c_void,
    numeric_type: *const FfiNumericType,
) -> *const CalcNode {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CalcNodeBuildEntry);
    crate::abort_on_panic(|| {
        handle(CalcNode::NonMathFunction {
            value: unsafe { RetainedStyleValue::from_shell_pointer(value) },
            numeric_type: unsafe { &*numeric_type }.to_calc(),
        })
    })
}

/// # Safety
/// `node` must be a valid transferred handle; this releases it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_calc_node_release(node: *const CalcNode) {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CalcNodeRetainReleaseEntry);
    crate::abort_on_panic(|| drop(unsafe { Arc::from_raw(node) }));
}

/// Retains an additional strong reference to a calculation node, so the C++
/// side can take ownership of a borrowed node.
///
/// # Safety
/// `node` must be a valid calculation node pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_calc_node_retain(node: *const CalcNode) {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CalcNodeRetainReleaseEntry);
    crate::abort_on_panic(|| unsafe { Arc::increment_strong_count(node) });
}

/// https://drafts.csswg.org/css-values-4/#determine-the-type-of-a-calculation
/// Determines the type of the calculation rooted at `node` under the given
/// resolve-as target. The result's valid flag is false when the tree does not
/// type-check.
///
/// # Safety
/// `node` must be a valid calculation node pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_calc_node_determine_type(
    node: *const CalcNode,
    has_percentages_resolve_as: bool,
    resolve_as_is_number: bool,
    resolve_as_base: u8,
) -> FfiNumericType {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CalcNodeQueryEntry);
    crate::abort_on_panic(|| {
        let resolve_as = resolve_as_from_fields(has_percentages_resolve_as, resolve_as_is_number, resolve_as_base);
        let percentage_leaf_type = percentage_leaf_type_for(resolve_as);
        FfiNumericType::from_calc(unsafe { &*node }.numeric_type(&percentage_leaf_type))
    })
}

/// Whether any leaf of the calculation is a percentage.
///
/// # Safety
/// `node` must be a valid calculation node pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_calc_node_contains_percentage(node: *const CalcNode) -> bool {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CalcNodeQueryEntry);
    crate::abort_on_panic(|| unsafe { &*node }.contains_percentage())
}

/// What percentages resolve against in the surrounding context.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum ResolveAs {
    Number,
    /// A base type index, in the numeric type order.
    Base(u8),
}

/// The inputs a calculation evaluation takes from the surrounding context.
pub(crate) struct CalcEvaluationContext<'a> {
    /// The type a percentage leaf takes in this context.
    pub percentage_leaf_type: &'a CalcNumericType,
    /// What percentages resolve against, when they resolve against another
    /// type; percentage leaves cannot evaluate while this is set.
    pub resolve_as: Option<ResolveAs>,
    /// The value percentages resolve against, when known.
    pub percentage_basis: Option<CalcNumericValue>,
    pub length_resolution: LengthResolution<'a>,
    /// Produces the random base value for a random() node's sharing options,
    /// when the resolution context can (it needs per-element state).
    pub random_base_value: Option<RandomBaseValueResolver<'a>>,
}

/// Produces the random base value for a random() node's sharing options.
pub(crate) type RandomBaseValueResolver<'a> = &'a dyn Fn(&RetainedStyleValue) -> Option<f64>;

/// The C++ seams the simplification needs: resolving a non-math function to a
/// calculation subtree, and looking up a relative-color channel value.
pub(crate) struct CalcSimplifyCallbacks<'a> {
    pub resolve_non_math_function: &'a dyn Fn(&RetainedStyleValue) -> Option<Arc<CalcNode>>,
    pub resolve_channel_keyword: &'a dyn Fn(u8) -> Option<f64>,
    /// Absolutizes a random() node's sharing options at computed-value time,
    /// fixing its per-element random base value; None keeps the original.
    pub absolutize_random_sharing: &'a dyn Fn(&RetainedStyleValue) -> Option<RetainedStyleValue>,
}

fn is_canonical_unit(value: CalcNumericValue) -> bool {
    // NB: Compare unit codes, not ratios: the "x" resolution unit shares dppx's
    //     ratio of one but is an alias that must still convert to dppx.
    match value {
        CalcNumericValue::Number { .. } | CalcNumericValue::Percentage(..) => true,
        CalcNumericValue::Length { unit, .. } => {
            unit == canonical_unit_code(&crate::style_compute::LENGTH_UNIT_CANONICAL_PX_RATIOS)
        }
        CalcNumericValue::Angle { unit, .. } => unit == canonical_unit_code(&ANGLE_UNIT_CANONICAL_RATIOS),
        CalcNumericValue::Flex { unit, .. } => unit == canonical_unit_code(&FLEX_UNIT_CANONICAL_RATIOS),
        CalcNumericValue::Frequency { unit, .. } => unit == canonical_unit_code(&FREQUENCY_UNIT_CANONICAL_RATIOS),
        CalcNumericValue::Resolution { unit, .. } => unit == canonical_unit_code(&RESOLUTION_UNIT_CANONICAL_RATIOS),
        CalcNumericValue::Time { unit, .. } => unit == canonical_unit_code(&TIME_UNIT_CANONICAL_RATIOS),
    }
}

impl CalcNode {
    /// Mirrors try_get_value_with_canonical_unit: a numeric child in its
    /// canonical unit whose percentages are resolved, as an evaluation result.
    fn try_canonical_result(&self, context: &CalcEvaluationContext) -> Option<CalcResult> {
        let CalcNode::Numeric(value) = self else {
            return None;
        };
        // Can't run with non-canonical units or unresolved percentages.
        // Simplification has already attempted to resolve both.
        if !is_canonical_unit(*value)
            || (matches!(value, CalcNumericValue::Percentage(..)) && context.resolve_as.is_some())
        {
            return None;
        }
        Some(CalcResult {
            value: value.to_canonical_number(context.length_resolution),
            numeric_type: self.numeric_type(context.percentage_leaf_type),
        })
    }

    /// Mirrors try_get_number: a plain number leaf.
    fn try_number(&self) -> Option<f64> {
        match self {
            CalcNode::Numeric(CalcNumericValue::Number { value, .. }) => Some(*value),
            _ => None,
        }
    }

    /// Collapses a function node whose children have already simplified to
    /// canonical numeric leaves, mirroring the run_operation_if_possible
    /// implementations. Returns None when the node cannot collapse (also for
    /// random(), which stays with the C++ per-element state).
    pub(crate) fn run_operation(&self, context: &CalcEvaluationContext) -> Option<CalcResult> {
        let leaf_type = context.percentage_leaf_type;
        // https://drafts.csswg.org/css-values-4/#calc-ieee
        // Any operation with at least one NaN argument produces NaN.
        let fold_min_or_max = |children: &[Arc<CalcNode>], is_min: bool| -> Option<CalcResult> {
            let mut result: Option<CalcResult> = None;
            for child in children {
                let child_value = child.try_canonical_result(context)?;
                match result {
                    None => result = Some(child_value),
                    Some(previous) => {
                        let consistent_type = previous.numeric_type?.added_to(&child_value.numeric_type?)?;
                        // Strict comparisons, so ties (including 0 vs -0) keep the earlier child.
                        let replace = if is_min {
                            child_value.value < previous.value
                        } else {
                            child_value.value > previous.value
                        };
                        let value = if child_value.value.is_nan() || previous.value.is_nan() {
                            f64::NAN
                        } else if replace {
                            child_value.value
                        } else {
                            previous.value
                        };
                        result = Some(CalcResult {
                            value,
                            numeric_type: Some(consistent_type),
                        });
                    }
                }
            }
            result
        };

        match self {
            // https://drafts.csswg.org/css-values-4/#funcdef-min
            CalcNode::Min(children) => fold_min_or_max(children, true),
            // https://drafts.csswg.org/css-values-4/#funcdef-max
            CalcNode::Max(children) => fold_min_or_max(children, false),
            // https://drafts.csswg.org/css-values-4/#funcdef-clamp
            CalcNode::Clamp { min, center, max } => {
                let min_result = min.try_canonical_result(context)?;
                let center_result = center.try_canonical_result(context)?;
                let max_result = max.try_canonical_result(context)?;
                // The first consistency must hold; the second stays part of the
                // result, exactly as in the C++ implementation.
                let first = min_result.numeric_type?.added_to(&center_result.numeric_type?)?;
                let numeric_type = first.added_to(&max_result.numeric_type?);
                let value = if min_result.value.is_nan() || center_result.value.is_nan() || max_result.value.is_nan() {
                    f64::NAN
                } else {
                    // Branch-based min/max, so ties (including 0 vs -0) keep the first
                    // operand; f64::min and f64::max may lose the zero sign on ties.
                    let inner = if max_result.value < center_result.value {
                        max_result.value
                    } else {
                        center_result.value
                    };
                    if min_result.value < inner {
                        inner
                    } else {
                        min_result.value
                    }
                };
                Some(CalcResult { value, numeric_type })
            }
            // https://drafts.csswg.org/css-values-4/#funcdef-abs
            CalcNode::Abs(child) => {
                let child_value = child.try_canonical_result(context)?;
                Some(CalcResult {
                    value: child_value.value.abs(),
                    numeric_type: child_value.numeric_type,
                })
            }
            // https://drafts.csswg.org/css-values-4/#funcdef-sign
            CalcNode::Sign(child) => {
                let CalcNode::Numeric(value) = &**child else {
                    return None;
                };
                let raw_value = match *value {
                    CalcNumericValue::Number { value, .. } => value,
                    CalcNumericValue::Percentage(value) => value / 100.0,
                    CalcNumericValue::Angle { value, .. }
                    | CalcNumericValue::Flex { value, .. }
                    | CalcNumericValue::Frequency { value, .. }
                    | CalcNumericValue::Length { value, .. }
                    | CalcNumericValue::Resolution { value, .. }
                    | CalcNumericValue::Time { value, .. } => value,
                };
                let numeric_type =
                    CalcNumericType::default().made_consistent_with(&child.numeric_type(leaf_type).unwrap_or_default());
                let sign = if raw_value.is_nan() {
                    f64::NAN
                } else if raw_value < 0.0 {
                    -1.0
                } else if raw_value > 0.0 {
                    1.0
                } else if raw_value.is_sign_negative() {
                    -0.0
                } else {
                    0.0
                };
                Some(CalcResult {
                    value: sign,
                    numeric_type,
                })
            }
            // https://drafts.csswg.org/css-values-4/#funcdef-sin (and cos, tan)
            CalcNode::Sin(child) | CalcNode::Cos(child) | CalcNode::Tan(child) => {
                let CalcNode::Numeric(value) = &**child else {
                    return None;
                };
                let radians = match *value {
                    CalcNumericValue::Number { value, .. } => value,
                    CalcNumericValue::Angle { value, unit } => {
                        (value * ANGLE_UNIT_CANONICAL_RATIOS[unit as usize]).to_radians()
                    }
                    _ => unreachable!("trigonometric argument must be a number or an angle"),
                };
                let result = match self {
                    CalcNode::Sin(..) => radians.sin(),
                    CalcNode::Cos(..) => radians.cos(),
                    _ => radians.tan(),
                };
                Some(CalcResult {
                    value: result,
                    numeric_type: CalcNumericType::default().made_consistent_with(&child.numeric_type(leaf_type)?),
                })
            }
            // https://drafts.csswg.org/css-values-4/#funcdef-asin (and acos, atan)
            CalcNode::Asin(child) | CalcNode::Acos(child) | CalcNode::Atan(child) => {
                let number = child.try_number()?;
                let normalize_angle = |radians: f64, min_degrees: f64, max_degrees: f64| {
                    let mut degrees = radians.to_degrees();
                    while degrees < min_degrees {
                        degrees += 360.0;
                    }
                    while degrees > max_degrees {
                        degrees -= 360.0;
                    }
                    degrees
                };
                let result = match self {
                    CalcNode::Asin(..) => normalize_angle(number.asin(), -90.0, 90.0),
                    CalcNode::Acos(..) => normalize_angle(number.acos(), 0.0, 180.0),
                    _ => normalize_angle(number.atan(), -90.0, 90.0),
                };
                let mut angle_type = CalcNumericType::default();
                angle_type.exponents[1] = Some(1);
                Some(CalcResult {
                    value: result,
                    numeric_type: angle_type.made_consistent_with(&child.numeric_type(leaf_type)?),
                })
            }
            // https://drafts.csswg.org/css-values-4/#funcdef-atan2
            CalcNode::Atan2 { y, x } => {
                let y_value = y.try_canonical_result(context)?;
                let x_value = x.try_canonical_result(context)?;
                let input_consistent_type = y_value.numeric_type?.added_to(&x_value.numeric_type?)?;
                let mut degrees = y_value.value.atan2(x_value.value).to_degrees();
                while degrees <= -180.0 {
                    degrees += 360.0;
                }
                while degrees > 180.0 {
                    degrees -= 360.0;
                }
                let mut angle_type = CalcNumericType::default();
                angle_type.exponents[1] = Some(1);
                Some(CalcResult {
                    value: degrees,
                    numeric_type: angle_type.made_consistent_with(&input_consistent_type),
                })
            }
            // https://drafts.csswg.org/css-values-4/#funcdef-pow
            CalcNode::Pow { base, exponent } => {
                let a = base.try_number()?;
                let b = exponent.try_number()?;
                let consistent_type = base
                    .numeric_type(leaf_type)?
                    .added_to(&exponent.numeric_type(leaf_type)?)?;
                Some(CalcResult {
                    value: a.powf(b),
                    numeric_type: Some(consistent_type),
                })
            }
            // https://drafts.csswg.org/css-values-4/#funcdef-sqrt
            CalcNode::Sqrt(child) => {
                let number = child.try_number()?;
                Some(CalcResult {
                    value: number.sqrt(),
                    numeric_type: CalcNumericType::default().made_consistent_with(&child.numeric_type(leaf_type)?),
                })
            }
            // https://drafts.csswg.org/css-values-4/#funcdef-log
            CalcNode::Log { value, base } => {
                let number = value.try_number()?;
                let base_number = base.try_number()?;
                Some(CalcResult {
                    value: number.ln() / base_number.ln(),
                    numeric_type: CalcNumericType::default().made_consistent_with(&value.numeric_type(leaf_type)?),
                })
            }
            // https://drafts.csswg.org/css-values-4/#funcdef-exp
            CalcNode::Exp(child) => {
                let number = child.try_number()?;
                Some(CalcResult {
                    value: number.exp(),
                    numeric_type: CalcNumericType::default().made_consistent_with(&child.numeric_type(leaf_type)?),
                })
            }
            // https://drafts.csswg.org/css-values-4/#funcdef-hypot
            CalcNode::Hypot(children) => {
                let mut consistent_type: Option<CalcNumericType> = None;
                let mut sum_of_squares = 0.0;
                for child in children {
                    let canonical_child = child.try_canonical_result(context)?;
                    consistent_type = Some(match consistent_type {
                        None => canonical_child.numeric_type?,
                        Some(previous) => previous.added_to(&canonical_child.numeric_type?)?,
                    });
                    sum_of_squares += canonical_child.value * canonical_child.value;
                }
                Some(CalcResult {
                    value: sum_of_squares.sqrt(),
                    numeric_type: Some(consistent_type?),
                })
            }
            // https://drafts.csswg.org/css-values-4/#funcdef-round
            CalcNode::Round {
                strategy,
                value,
                interval,
            } => {
                let maybe_a = value.try_canonical_result(context)?;
                let maybe_b = interval.try_canonical_result(context)?;
                let consistent_type = maybe_a.numeric_type?.made_consistent_with(&maybe_b.numeric_type?)?;
                let a = maybe_a.value;
                let b = maybe_b.value;
                let result_with = |value: f64| {
                    Some(CalcResult {
                        value,
                        numeric_type: Some(consistent_type),
                    })
                };

                // Rounding strategy codes follow the generated C++ RoundingStrategy
                // order: down, nearest, to-zero, up.
                let (down, nearest, to_zero, up) = (0u8, 1u8, 2u8, 3u8);

                // https://drafts.csswg.org/css-values-4/#round-infinities
                // In round(A, B), if B is 0, the result is NaN. If A and B are both infinite, the result is NaN.
                if b == 0.0 || (a.is_infinite() && b.is_infinite()) {
                    return result_with(f64::NAN);
                }
                // If A is infinite but B is finite, the result is the same infinity.
                if a.is_infinite() && b.is_finite() {
                    return result_with(a);
                }
                // If A is finite but B is infinite, the result depends on the <rounding-strategy> and the sign of A:
                if a.is_finite() && b.is_infinite() {
                    let negative_zero = a.is_sign_negative();
                    let signed_zero = if negative_zero { -0.0 } else { 0.0 };
                    let result = match *strategy {
                        // nearest, to-zero: If A is positive or 0+, return 0+. Otherwise, return 0-.
                        s if s == nearest || s == to_zero => signed_zero,
                        // up: If A is positive (not zero), return +Infinity; else the signed zero.
                        s if s == up => {
                            if a > 0.0 {
                                f64::INFINITY
                            } else {
                                signed_zero
                            }
                        }
                        // down: If A is negative (not zero), return -Infinity; else the signed zero.
                        s if s == down => {
                            if a < 0.0 {
                                f64::NEG_INFINITY
                            } else {
                                signed_zero
                            }
                        }
                        _ => unreachable!("invalid rounding strategy {strategy}"),
                    };
                    return result_with(result);
                }

                // If A is exactly equal to an integer multiple of B, round() resolves to A exactly
                // (preserving whether A is 0- or 0+, if relevant).
                if a % b == 0.0 {
                    return Some(maybe_a);
                }

                // Otherwise, there are two integer multiples of B that are potentially "closest"
                // to A, lower B which is closer to −∞ and upper B which is closer to +∞. The
                // following <rounding-strategy>s dictate how to choose between them:

                // FIXME: If lower B would be zero, it is specifically equal to 0⁺;
                //        if upper B would be zero, it is specifically equal to 0⁻.
                let lower_b = (a / b).floor() * b;
                let upper_b = (a / b).ceil() * b;
                let rounded = match *strategy {
                    // nearest: whichever of lower B and upper B has the smallest absolute
                    // difference from A; ties choose upper B.
                    s if s == nearest => {
                        if (upper_b - a).abs() <= (lower_b - a).abs() {
                            upper_b
                        } else {
                            lower_b
                        }
                    }
                    s if s == up => upper_b,
                    s if s == down => lower_b,
                    // to-zero: whichever of lower B and upper B has the smallest absolute
                    // difference from 0.
                    s if s == to_zero => {
                        if upper_b.abs() < lower_b.abs() {
                            upper_b
                        } else {
                            lower_b
                        }
                    }
                    _ => unreachable!("invalid rounding strategy {strategy}"),
                };
                result_with(rounded)
            }
            // https://drafts.csswg.org/css-values-4/#funcdef-mod (and rem)
            CalcNode::Mod { value, modulus }
            | CalcNode::Rem {
                value,
                divisor: modulus,
            } => {
                let numerator = value.try_canonical_result(context)?;
                let denominator = modulus.try_canonical_result(context)?;
                // The arguments must have the same type, not merely consistent ones.
                if numerator.numeric_type != denominator.numeric_type {
                    return None;
                }
                let result = if matches!(self, CalcNode::Mod { .. }) {
                    numerator.value - denominator.value * (numerator.value / denominator.value).floor()
                } else {
                    numerator.value % denominator.value
                };
                Some(CalcResult {
                    value: result,
                    numeric_type: numerator.numeric_type,
                })
            }
            // https://drafts.csswg.org/css-values-5/#calculate-a-progress-function
            CalcNode::Progress {
                no_clamp,
                progress,
                from,
                to,
            } => {
                let value = progress.try_canonical_result(context)?;
                let start = from.try_canonical_result(context)?;
                let end = to.try_canonical_result(context)?;
                let numeric_type = self.numeric_type(leaf_type);

                // If the progress start value and progress end value are different values
                if start != end {
                    // (progress value - progress start value) / (progress end value - progress start value),
                    // clamped to the [0,1] range if no-clamp is not specified.
                    let mut result = (value.value - start.value) / (end.value - start.value);
                    if !no_clamp {
                        result = result.clamp(0.0, 1.0);
                    }
                    return Some(CalcResult {
                        value: result,
                        numeric_type,
                    });
                }

                // If the progress start value and progress end value are the same value:
                // 0 if no-clamp is not specified; otherwise 0, -Infinity, or +Infinity, depending
                // on whether progress value is equal to, less than, or greater than the shared value.
                let result = if !no_clamp || value.value == start.value {
                    0.0
                } else if value.value < start.value {
                    f64::NEG_INFINITY
                } else {
                    f64::INFINITY
                };
                Some(CalcResult {
                    value: result,
                    numeric_type,
                })
            }
            // https://drafts.csswg.org/css-values-5/#random-evaluation
            CalcNode::Random {
                min,
                max,
                step,
                sharing,
                ..
            } => {
                let random_base_value = context.random_base_value.as_ref()?(sharing)?;
                let minimum = min.try_canonical_result(context)?;
                let maximum = max.try_canonical_result(context)?;
                let minimum_value = minimum.value;
                let mut maximum_value = maximum.value;
                let mut step_value = 0.0;
                if let Some(step) = step {
                    step_value = step.try_canonical_result(context)?.value;
                }
                let numeric_type = self.numeric_type(leaf_type);
                let result = |value: f64| Some(CalcResult { value, numeric_type });

                // https://drafts.csswg.org/css-values-5/#random-infinities
                // If the maximum value is less than the minimum value, it behaves as if it's
                // equal to the minimum value.
                if maximum_value < minimum_value {
                    maximum_value = minimum_value;
                }
                // In random(A, B), if A is infinite, the result is infinite. If A is finite,
                // but the difference between A and B is infinite, the result is NaN. If C is
                // infinite, the result is A.
                if minimum_value.is_infinite() {
                    return result(f64::INFINITY);
                }
                if maximum_value.is_infinite() {
                    return result(f64::NAN);
                }
                if step_value.is_infinite() {
                    return result(minimum_value);
                }
                // As usual for math functions, if any argument calculation is NaN, the result is NaN.
                if minimum_value.is_nan() || maximum_value.is_nan() || step_value.is_nan() {
                    return result(f64::NAN);
                }
                // If C is negative, zero, or close enough to zero that the step multiplier
                // range would be infinite, the step must be ignored.
                let has_step = step_value > f64::from(f32::EPSILON) * 1000.0;
                // For a random() function with min and max, but no step: min + R * (max - min).
                if !has_step {
                    return result(minimum_value + random_base_value * (maximum_value - minimum_value));
                }
                // Let epsilon be step / 1000. Let N be the largest integer such that
                // min + N * step is less than or equal to max; if N is not within epsilon of
                // max but N+1 is, use N+1. Let step index be a random integer less than N+1.
                let epsilon = step_value / 1000.0;
                let mut n = ((maximum_value - minimum_value) / step_value).floor();
                if (maximum_value - (n * step_value + minimum_value)).abs() > epsilon
                    && (maximum_value - ((n + 1.0) * step_value + minimum_value)).abs() < epsilon
                {
                    n += 1.0;
                }
                let step_index = ((n + 1.0) * random_base_value).floor();
                // Let value be min + step index * step; if step index is N and value is
                // within epsilon of max, return max.
                let value = minimum_value + step_index * step_value;
                if step_index == n && (maximum_value - value).abs() < epsilon {
                    return result(maximum_value);
                }
                result(value)
            }
            // Sums, products, negation and inversion collapse in the
            // simplification driver; leaves and non-math functions have no
            // operation to run.
            _ => None,
        }
    }
}

impl CalcNumericType {
    /// https://drafts.css-houdini.org/css-typed-om-1/#cssnumericvalue-match
    /// The single base type whose entry is 1 while all other entries are 0.
    pub(crate) fn entry_with_value_1_while_all_others_are_0(&self) -> Option<usize> {
        let mut found = None;
        for i in 0..BASE_TYPE_COUNT {
            match self.exponents[i] {
                None | Some(0) => {}
                Some(1) if found.is_none() => found = Some(i),
                _ => return None,
            }
        }
        found
    }

    fn hint_matches(&self, resolve_as: ResolveAs) -> bool {
        match (self.percent_hint, resolve_as) {
            (None, _) => true,
            (Some(hint), ResolveAs::Base(base)) => hint == base,
            (Some(..), ResolveAs::Number) => false,
        }
    }

    /// A type matches a dimension if its only non-zero entry is that base type
    /// with value 1, and the percent hint is compatible with the context.
    pub(crate) fn matches_dimension(&self, base: usize, resolve_as: Option<ResolveAs>) -> bool {
        if self.entry_with_value_1_while_all_others_are_0() != Some(base) {
            return false;
        }
        match resolve_as {
            Some(resolve_as) => self.hint_matches(resolve_as),
            None => self.percent_hint.is_none(),
        }
    }

    /// A type matches <percentage> if its only non-zero entry is percent with
    /// value 1, and its percent hint is null or percent.
    pub(crate) fn matches_percentage(&self) -> bool {
        if self.percent_hint.is_some() && self.percent_hint != Some(BASE_TYPE_PERCENT as u8) {
            return false;
        }
        self.entry_with_value_1_while_all_others_are_0() == Some(BASE_TYPE_PERCENT)
    }

    /// A type matches <number> if it has no non-zero entries, with the hint
    /// compatible when percentages resolve against a non-number type.
    pub(crate) fn matches_number(&self, resolve_as: Option<ResolveAs>) -> bool {
        for i in 0..BASE_TYPE_COUNT {
            if self.exponents[i].is_some() && self.exponents[i] != Some(0) {
                return false;
            }
        }
        // The percent hint must be null when the context does not allow percentages, percent when
        // they resolve as numbers, and null or the resolved type otherwise, as in matches_number.
        match resolve_as {
            Some(ResolveAs::Base(base)) => self.percent_hint.is_none() || self.percent_hint == Some(base),
            Some(ResolveAs::Number) => {
                self.percent_hint.is_none() || self.percent_hint == Some(BASE_TYPE_PERCENT as u8)
            }
            None => self.percent_hint.is_none(),
        }
    }
}

/// The canonical unit code of each dimension: the index whose ratio is one.
fn canonical_unit_code(ratios: &[f64]) -> u8 {
    ratios
        .iter()
        .position(|&ratio| ratio == 1.0)
        .expect("dimension has a canonical unit") as u8
}

fn make_calc_result_node(result: &CalcResult, resolve_as: Option<ResolveAs>) -> Option<CalcNode> {
    // Mirrors make_calculation_node: express the result in its type's
    // canonical unit, or fail when the type matches nothing expressible.
    let numeric_type = result.numeric_type.as_ref()?;
    let value = result.value;
    let numeric = if numeric_type.matches_number(resolve_as) {
        CalcNumericValue::Number { value, number_type: 0 }
    } else if numeric_type.matches_percentage() {
        CalcNumericValue::Percentage(value)
    } else if numeric_type.matches_dimension(1, resolve_as) {
        CalcNumericValue::Angle {
            value,
            unit: canonical_unit_code(&ANGLE_UNIT_CANONICAL_RATIOS),
        }
    } else if numeric_type.matches_dimension(5, resolve_as) {
        CalcNumericValue::Flex {
            value,
            unit: canonical_unit_code(&FLEX_UNIT_CANONICAL_RATIOS),
        }
    } else if numeric_type.matches_dimension(3, resolve_as) {
        CalcNumericValue::Frequency {
            value,
            unit: canonical_unit_code(&FREQUENCY_UNIT_CANONICAL_RATIOS),
        }
    } else if numeric_type.matches_dimension(0, resolve_as) {
        CalcNumericValue::Length {
            value,
            unit: canonical_unit_code(&crate::style_compute::LENGTH_UNIT_CANONICAL_PX_RATIOS),
        }
    } else if numeric_type.matches_dimension(4, resolve_as) {
        CalcNumericValue::Resolution {
            value,
            unit: canonical_unit_code(&RESOLUTION_UNIT_CANONICAL_RATIOS),
        }
    } else if numeric_type.matches_dimension(2, resolve_as) {
        CalcNumericValue::Time {
            value,
            unit: canonical_unit_code(&TIME_UNIT_CANONICAL_RATIOS),
        }
    } else {
        return None;
    };
    Some(CalcNode::Numeric(numeric))
}

/// Whether two numeric leaves are expressed in the same unit, so they can be
/// merged or compared directly.
fn same_unit(a: CalcNumericValue, b: CalcNumericValue) -> bool {
    match (a, b) {
        (CalcNumericValue::Number { .. }, CalcNumericValue::Number { .. }) => true,
        (CalcNumericValue::Percentage(..), CalcNumericValue::Percentage(..)) => true,
        (CalcNumericValue::Angle { unit: a, .. }, CalcNumericValue::Angle { unit: b, .. })
        | (CalcNumericValue::Flex { unit: a, .. }, CalcNumericValue::Flex { unit: b, .. })
        | (CalcNumericValue::Frequency { unit: a, .. }, CalcNumericValue::Frequency { unit: b, .. })
        | (CalcNumericValue::Length { unit: a, .. }, CalcNumericValue::Length { unit: b, .. })
        | (CalcNumericValue::Resolution { unit: a, .. }, CalcNumericValue::Resolution { unit: b, .. })
        | (CalcNumericValue::Time { unit: a, .. }, CalcNumericValue::Time { unit: b, .. }) => a == b,
        _ => false,
    }
}

impl CalcNumericValue {
    fn raw(self) -> f64 {
        match self {
            CalcNumericValue::Number { value, .. } | CalcNumericValue::Percentage(value) => value,
            CalcNumericValue::Angle { value, .. }
            | CalcNumericValue::Flex { value, .. }
            | CalcNumericValue::Frequency { value, .. }
            | CalcNumericValue::Length { value, .. }
            | CalcNumericValue::Resolution { value, .. }
            | CalcNumericValue::Time { value, .. } => value,
        }
    }

    fn with_raw(self, raw: f64) -> CalcNumericValue {
        match self {
            CalcNumericValue::Number { number_type, .. } => CalcNumericValue::Number {
                value: raw,
                number_type,
            },
            CalcNumericValue::Percentage(..) => CalcNumericValue::Percentage(raw),
            CalcNumericValue::Angle { unit, .. } => CalcNumericValue::Angle { value: raw, unit },
            CalcNumericValue::Flex { unit, .. } => CalcNumericValue::Flex { value: raw, unit },
            CalcNumericValue::Frequency { unit, .. } => CalcNumericValue::Frequency { value: raw, unit },
            CalcNumericValue::Length { unit, .. } => CalcNumericValue::Length { value: raw, unit },
            CalcNumericValue::Resolution { unit, .. } => CalcNumericValue::Resolution { value: raw, unit },
            CalcNumericValue::Time { unit, .. } => CalcNumericValue::Time { value: raw, unit },
        }
    }

    /// The value converted to its dimension's canonical unit, when possible
    /// without extra information (relative lengths need the context).
    fn to_canonical_value(self, length_resolution: LengthResolution) -> Option<CalcNumericValue> {
        if is_canonical_unit(self) {
            return Some(self);
        }
        match self {
            CalcNumericValue::Number { .. } | CalcNumericValue::Percentage(..) => Some(self),
            CalcNumericValue::Angle { value, unit } => Some(CalcNumericValue::Angle {
                value: value * ANGLE_UNIT_CANONICAL_RATIOS[unit as usize],
                unit: canonical_unit_code(&ANGLE_UNIT_CANONICAL_RATIOS),
            }),
            CalcNumericValue::Flex { value, unit } => Some(CalcNumericValue::Flex {
                value: value * FLEX_UNIT_CANONICAL_RATIOS[unit as usize],
                unit: canonical_unit_code(&FLEX_UNIT_CANONICAL_RATIOS),
            }),
            CalcNumericValue::Frequency { value, unit } => Some(CalcNumericValue::Frequency {
                value: value * FREQUENCY_UNIT_CANONICAL_RATIOS[unit as usize],
                unit: canonical_unit_code(&FREQUENCY_UNIT_CANONICAL_RATIOS),
            }),
            CalcNumericValue::Resolution { value, unit } => Some(CalcNumericValue::Resolution {
                value: value * RESOLUTION_UNIT_CANONICAL_RATIOS[unit as usize],
                unit: canonical_unit_code(&RESOLUTION_UNIT_CANONICAL_RATIOS),
            }),
            CalcNumericValue::Time { value, unit } => Some(CalcNumericValue::Time {
                value: value * TIME_UNIT_CANONICAL_RATIOS[unit as usize],
                unit: canonical_unit_code(&TIME_UNIT_CANONICAL_RATIOS),
            }),
            CalcNumericValue::Length { value, unit } => {
                let px = canonical_unit_code(&crate::style_compute::LENGTH_UNIT_CANONICAL_PX_RATIOS);
                let ratio = crate::style_compute::LENGTH_UNIT_CANONICAL_PX_RATIOS[unit as usize];
                if ratio.is_finite() {
                    return Some(CalcNumericValue::Length {
                        value: value * ratio,
                        unit: px,
                    });
                }
                let context = length_resolution.context?;
                let result = crate::style_compute::absolutize_length_for_calc(value, unit as usize, context);
                if result.handled {
                    return Some(CalcNumericValue::Length {
                        value: result.px,
                        unit: px,
                    });
                }
                let resolved = length_resolution.fallback.and_then(|fallback| fallback(value, unit))?;
                Some(CalcNumericValue::Length {
                    value: resolved,
                    unit: px,
                })
            }
        }
    }
}

impl CalcNode {
    /// https://drafts.csswg.org/css-values-4/#calc-simplification
    /// Simplifies a calculation tree, mirroring the C++ driver: leaves resolve
    /// to canonical units and against the percentage basis, operator nodes
    /// simplify their children and collapse where possible, and sums and
    /// products flatten and fold.
    pub(crate) fn simplify(
        self: &Arc<Self>,
        context: &CalcEvaluationContext,
        callbacks: &CalcSimplifyCallbacks,
    ) -> Arc<CalcNode> {
        // 1. If root is a numeric value:
        if let CalcNode::Numeric(value) = &**self {
            // 1. If root is a percentage that will be resolved against another value, and there is enough
            //    information available to resolve it, do so, and express the resulting numeric value in the
            //    appropriate canonical unit. Return the value.
            if let (CalcNumericValue::Percentage(percentage), Some(..), Some(basis)) =
                (*value, context.resolve_as, context.percentage_basis)
            {
                if let Some(canonical_basis) = basis.to_canonical_value(context.length_resolution) {
                    return shared(CalcNode::Numeric(
                        canonical_basis.with_raw(canonical_basis.raw() * percentage / 100.0),
                    ));
                }
                return self.clone();
            }

            // 2. If root is a dimension that is not expressed in its canonical unit, and there is enough
            //    information available to convert it to the canonical unit, do so, and return the value.
            if !is_canonical_unit(*value)
                && let Some(canonical) = value.to_canonical_value(context.length_resolution)
            {
                return shared(CalcNode::Numeric(canonical));
            }

            // 3. If root is a <calc-keyword> that can be resolved, return what it resolves to,
            //    simplified.
            // NOTE: We already resolve our `<calc-keyword>`s at parse-time.
            // FIXME: Revisit this once we support any keywords that need resolving later.

            // 4. Otherwise, return root.
            return self.clone();
        }

        // 2. If root is any other leaf node (not an operator node): if there is enough information
        //    available to determine its numeric value, return its value in its canonical unit.
        if let CalcNode::NonMathFunction { value, .. } = &**self {
            if let Some(resolved) = (callbacks.resolve_non_math_function)(value) {
                return resolved;
            }
            return self.clone();
        }
        // https://drafts.csswg.org/css-color-5/#relative-color
        if let CalcNode::ChannelKeyword(channel) = &**self {
            if let Some(resolved) = (callbacks.resolve_channel_keyword)(*channel) {
                return shared(CalcNode::Numeric(CalcNumericValue::Number {
                    value: resolved,
                    number_type: 0,
                }));
            }
            return self.clone();
        }

        // 3. At this point, root is an operator node. Simplify all the calculation children of root.
        let root = self.with_simplified_children(context, callbacks);

        // 4. If root is an operator node that's not one of the calc-operator nodes, and all of its
        //    calculation children are numeric values with enough information to compute the operation,
        //    return the result of running root's operation, expressed in the result's canonical unit.
        if let Some(result) = root.run_operation(context) {
            if let Some(node) = make_calc_result_node(&result, context.resolve_as) {
                return shared(node);
            }
            return root;
        }

        match &*root {
            // 5. If root is a Min or Max node, attempt to partially simplify it: combine numeric
            //    children that share a unit, and unwrap a singleton.
            CalcNode::Min(children) | CalcNode::Max(children) => {
                let is_min = matches!(&*root, CalcNode::Min(..));
                let mut simplified_children: Vec<Arc<CalcNode>> = Vec::with_capacity(children.len());
                for child in children {
                    // NOTE: When a percentage will resolve against another value we can't compare
                    //       it with other children yet.
                    let merged = match &**child {
                        CalcNode::Numeric(child_value)
                            if !simplified_children.is_empty()
                                && (!matches!(child_value, CalcNumericValue::Percentage(..))
                                    || context.resolve_as.is_none()) =>
                        {
                            let existing = simplified_children.iter_mut().find(|existing| {
                                matches!(&***existing, CalcNode::Numeric(existing_value) if same_unit(*existing_value, *child_value))
                            });
                            match existing {
                                Some(existing) => {
                                    let CalcNode::Numeric(existing_value) = &**existing else {
                                        unreachable!();
                                    };
                                    let replace = if is_min {
                                        child_value.raw() < existing_value.raw()
                                    } else {
                                        child_value.raw() > existing_value.raw()
                                    };
                                    if replace {
                                        *existing = child.clone();
                                    }
                                    true
                                }
                                None => false,
                            }
                        }
                        _ => false,
                    };
                    if !merged {
                        simplified_children.push(child.clone());
                    }
                }
                if simplified_children.len() == 1 {
                    return simplified_children.remove(0);
                }
                if is_min {
                    shared(CalcNode::Min(simplified_children))
                } else {
                    shared(CalcNode::Max(simplified_children))
                }
            }
            // 6. If root is a Negate node:
            CalcNode::Negate(child) => {
                // 1. If root's child is a numeric value, return an equivalent numeric value with the value negated.
                // NB: Unary negation, not 0 - x, which would turn -0 into +0.
                if let CalcNode::Numeric(value) = &**child {
                    return shared(CalcNode::Numeric(value.with_raw(-value.raw())));
                }
                // 2. If root's child is a Negate node, return the child's child.
                if let CalcNode::Negate(inner) = &**child {
                    return inner.clone();
                }
                // AD-HOC: Convert negated sums into sums of negated nodes - see
                // https://github.com/w3c/csswg-drafts/issues/13020
                if let CalcNode::Sum(sum_children) = &**child {
                    let negated = sum_children
                        .iter()
                        .map(|sum_child| match &**sum_child {
                            CalcNode::Numeric(value) => shared(CalcNode::Numeric(value.with_raw(-value.raw()))),
                            CalcNode::Negate(inner) => inner.clone(),
                            _ => shared(CalcNode::Negate(sum_child.clone())),
                        })
                        .collect();
                    return shared(CalcNode::Sum(negated));
                }
                // 3. Return root.
                root
            }
            // 7. If root is an Invert node:
            CalcNode::Invert(child) => {
                // 1. If root's child is a number (not a percentage or dimension) return the reciprocal.
                if let CalcNode::Numeric(CalcNumericValue::Number { value: number, .. }) = &**child {
                    return shared(CalcNode::Numeric(CalcNumericValue::Number {
                        value: 1.0 / number,
                        number_type: 0,
                    }));
                }
                // 2. If root's child is an Invert node, return the child's child.
                if let CalcNode::Invert(inner) = &**child {
                    return inner.clone();
                }
                // 3. Return root.
                root
            }
            // 8. If root is a Sum node: flatten nested sums, combine same-unit numeric children,
            //    and unwrap a singleton.
            CalcNode::Sum(children) => {
                let mut flattened: Vec<Arc<CalcNode>> = Vec::with_capacity(children.len());
                for child in children {
                    if let CalcNode::Sum(nested) = &**child {
                        flattened.extend(nested.iter().cloned());
                    } else {
                        flattened.push(child.clone());
                    }
                }
                let mut summed: Vec<Arc<CalcNode>> = Vec::with_capacity(flattened.len());
                for child in flattened {
                    let merged = match &*child {
                        CalcNode::Numeric(child_value) => {
                            let existing = summed.iter_mut().find(|existing| {
                                matches!(&***existing, CalcNode::Numeric(existing_value) if same_unit(*existing_value, *child_value))
                            });
                            match existing {
                                Some(existing) => {
                                    let CalcNode::Numeric(existing_value) = &**existing else {
                                        unreachable!();
                                    };
                                    *existing = shared(CalcNode::Numeric(
                                        existing_value.with_raw(existing_value.raw() + child_value.raw()),
                                    ));
                                    true
                                }
                                None => false,
                            }
                        }
                        _ => false,
                    };
                    if !merged {
                        summed.push(child);
                    }
                }
                if summed.len() == 1 {
                    return summed.remove(0);
                }
                shared(CalcNode::Sum(summed))
            }
            // 9. If root is a Product node: flatten nested products, fold plain numbers together,
            //    distribute a number over an all-numeric sum, and try to accumulate the whole
            //    product into a single numeric value.
            CalcNode::Product(children) => {
                let mut flattened: Vec<Arc<CalcNode>> = Vec::with_capacity(children.len());
                for child in children {
                    if let CalcNode::Product(nested) = &**child {
                        flattened.extend(nested.iter().cloned());
                    } else {
                        flattened.push(child.clone());
                    }
                }

                let mut number_index: Option<usize> = None;
                let mut i = 0;
                while i < flattened.len() {
                    if let CalcNode::Numeric(CalcNumericValue::Number { value: number, .. }) = &*flattened[i] {
                        match number_index {
                            None => number_index = Some(i),
                            Some(index) => {
                                let CalcNode::Numeric(CalcNumericValue::Number { value: existing, .. }) =
                                    &*flattened[index]
                                else {
                                    unreachable!();
                                };
                                flattened[index] = shared(CalcNode::Numeric(CalcNumericValue::Number {
                                    value: existing * number,
                                    number_type: 0,
                                }));
                                flattened.remove(i);
                                continue;
                            }
                        }
                    }
                    i += 1;
                }

                if flattened.len() == 2 {
                    let multiplier_and_sum = match (&*flattened[0], &*flattened[1]) {
                        (CalcNode::Numeric(CalcNumericValue::Number { value: multiplier, .. }), CalcNode::Sum(sum)) => {
                            Some((*multiplier, sum))
                        }
                        (CalcNode::Sum(sum), CalcNode::Numeric(CalcNumericValue::Number { value: multiplier, .. })) => {
                            Some((*multiplier, sum))
                        }
                        _ => None,
                    };
                    if let Some((multiplier, sum)) = multiplier_and_sum {
                        let all_numeric = sum.iter().all(|child| matches!(&**child, CalcNode::Numeric(..)));
                        if all_numeric {
                            let multiplied = sum
                                .iter()
                                .map(|child| {
                                    let CalcNode::Numeric(value) = &**child else {
                                        unreachable!();
                                    };
                                    shared(CalcNode::Numeric(value.with_raw(value.raw() * multiplier)))
                                })
                                .collect();
                            return shared(CalcNode::Sum(multiplied)).simplify(context, callbacks);
                        }
                    }
                }

                // Accumulate the whole product when every child is a canonical numeric value or
                // the inversion of one.
                // AD-HOC: The spec doesn't cover how to handle unresolved percentages, to handle
                //         this we force percentages back to the percent type (e.g.
                //         { hint: None, "percent" → 1 } rather than { hint: length, "length" → 1 }),
                //         this avoids treating the value as an absolute value expressed in
                //         canonical units rather than a percent. The correct numeric type is still
                //         calculated for the simplified node afterwards.
                //         See spec issue: https://github.com/w3c/csswg-drafts/issues/11588
                let mut accumulated: Option<CalcResult> = None;
                let mut is_valid = true;
                let mut accumulate = |leaf: &CalcNode, invert: bool| -> bool {
                    let CalcNode::Numeric(value) = leaf else {
                        return false;
                    };
                    if !is_canonical_unit(*value) {
                        return false;
                    }
                    let mut numeric_type = leaf.numeric_type(context.percentage_leaf_type);
                    if matches!(value, CalcNumericValue::Percentage(..)) {
                        let mut percent_type = CalcNumericType::default();
                        percent_type.exponents[BASE_TYPE_PERCENT] = Some(1);
                        numeric_type = Some(percent_type);
                    }
                    let mut child_value = CalcResult {
                        value: value.to_canonical_number(context.length_resolution),
                        numeric_type,
                    };
                    if invert {
                        child_value.invert();
                    }
                    match &mut accumulated {
                        Some(accumulated) => accumulated.multiply_by(&child_value),
                        None => accumulated = Some(child_value),
                    }
                    accumulated.as_ref().is_some_and(|result| result.numeric_type.is_some())
                };
                for child in &flattened {
                    let ok = match &**child {
                        CalcNode::Numeric(..) => accumulate(child, false),
                        CalcNode::Invert(inner) => matches!(&**inner, CalcNode::Numeric(..)) && accumulate(inner, true),
                        _ => false,
                    };
                    if !ok {
                        is_valid = false;
                        break;
                    }
                }
                if is_valid
                    && let Some(result) = &accumulated
                    && let Some(node) = make_calc_result_node(result, context.resolve_as)
                {
                    return shared(node);
                }
                shared(CalcNode::Product(flattened))
            }
            _ => root,
        }
    }

    /// Simplifies every child, rebuilding the node only when a child changed.
    fn with_simplified_children(
        self: &Arc<Self>,
        context: &CalcEvaluationContext,
        callbacks: &CalcSimplifyCallbacks,
    ) -> Arc<CalcNode> {
        let mut changed = false;
        let simplify_child = |child: &Arc<CalcNode>, changed: &mut bool| {
            let simplified = child.simplify(context, callbacks);
            if !Arc::ptr_eq(&simplified, child) {
                *changed = true;
            }
            simplified
        };
        let simplify_children = |children: &[Arc<CalcNode>], changed: &mut bool| {
            children
                .iter()
                .map(|child| simplify_child(child, changed))
                .collect::<Vec<_>>()
        };
        let rebuilt = match &**self {
            CalcNode::Sum(children) => CalcNode::Sum(simplify_children(children, &mut changed)),
            CalcNode::Product(children) => CalcNode::Product(simplify_children(children, &mut changed)),
            CalcNode::Min(children) => CalcNode::Min(simplify_children(children, &mut changed)),
            CalcNode::Max(children) => CalcNode::Max(simplify_children(children, &mut changed)),
            CalcNode::Hypot(children) => CalcNode::Hypot(simplify_children(children, &mut changed)),
            CalcNode::Negate(child) => CalcNode::Negate(simplify_child(child, &mut changed)),
            CalcNode::Invert(child) => CalcNode::Invert(simplify_child(child, &mut changed)),
            CalcNode::Abs(child) => CalcNode::Abs(simplify_child(child, &mut changed)),
            CalcNode::Sign(child) => CalcNode::Sign(simplify_child(child, &mut changed)),
            CalcNode::Sin(child) => CalcNode::Sin(simplify_child(child, &mut changed)),
            CalcNode::Cos(child) => CalcNode::Cos(simplify_child(child, &mut changed)),
            CalcNode::Tan(child) => CalcNode::Tan(simplify_child(child, &mut changed)),
            CalcNode::Asin(child) => CalcNode::Asin(simplify_child(child, &mut changed)),
            CalcNode::Acos(child) => CalcNode::Acos(simplify_child(child, &mut changed)),
            CalcNode::Atan(child) => CalcNode::Atan(simplify_child(child, &mut changed)),
            CalcNode::Sqrt(child) => CalcNode::Sqrt(simplify_child(child, &mut changed)),
            CalcNode::Exp(child) => CalcNode::Exp(simplify_child(child, &mut changed)),
            CalcNode::Clamp { min, center, max } => CalcNode::Clamp {
                min: simplify_child(min, &mut changed),
                center: simplify_child(center, &mut changed),
                max: simplify_child(max, &mut changed),
            },
            CalcNode::Progress {
                no_clamp,
                progress,
                from,
                to,
            } => CalcNode::Progress {
                no_clamp: *no_clamp,
                progress: simplify_child(progress, &mut changed),
                from: simplify_child(from, &mut changed),
                to: simplify_child(to, &mut changed),
            },
            CalcNode::Atan2 { y, x } => CalcNode::Atan2 {
                y: simplify_child(y, &mut changed),
                x: simplify_child(x, &mut changed),
            },
            CalcNode::Pow { base, exponent } => CalcNode::Pow {
                base: simplify_child(base, &mut changed),
                exponent: simplify_child(exponent, &mut changed),
            },
            CalcNode::Log { value, base } => CalcNode::Log {
                value: simplify_child(value, &mut changed),
                base: simplify_child(base, &mut changed),
            },
            CalcNode::Round {
                strategy,
                value,
                interval,
            } => CalcNode::Round {
                strategy: *strategy,
                value: simplify_child(value, &mut changed),
                interval: simplify_child(interval, &mut changed),
            },
            CalcNode::Mod { value, modulus } => CalcNode::Mod {
                value: simplify_child(value, &mut changed),
                modulus: simplify_child(modulus, &mut changed),
            },
            CalcNode::Rem { value, divisor } => CalcNode::Rem {
                value: simplify_child(value, &mut changed),
                divisor: simplify_child(divisor, &mut changed),
            },
            CalcNode::Random {
                min,
                max,
                step,
                sharing,
                ..
            } => {
                // The sharing options absolutize at computed-value time, fixing
                // the per-element random base value.
                let simplified_sharing = match (callbacks.absolutize_random_sharing)(sharing) {
                    Some(absolutized) => {
                        changed = true;
                        absolutized
                    }
                    None => sharing.clone_retained(),
                };
                CalcNode::Random {
                    min: simplify_child(min, &mut changed),
                    max: simplify_child(max, &mut changed),
                    step: step.as_ref().map(|step| simplify_child(step, &mut changed)),
                    sharing: simplified_sharing,
                }
            }
            CalcNode::Numeric(..) | CalcNode::ChannelKeyword(..) | CalcNode::NonMathFunction { .. } => {
                return self.clone();
            }
        };
        if changed { shared(rebuilt) } else { self.clone() }
    }
}

/// The C++ ValueType discriminants the range lookup needs, pinned by
/// static_asserts on the C++ side.
mod value_type {
    pub const ANGLE: u8 = 2;
    pub const FLEX: u8 = 15;
    pub const FREQUENCY: u8 = 21;
    pub const INTEGER: u8 = 24;
    pub const LENGTH: u8 = 25;
    pub const NUMBER: u8 = 27;
    pub const PERCENTAGE: u8 = 31;
    pub const RESOLUTION: u8 = 35;
    pub const TIME: u8 = 38;
}

/// The resolution inputs marshaled from the C++ CalculationResolutionContext.
#[repr(C)]
pub struct FfiCalcResolutionContext {
    /// The percentage basis: kind 0 = none, then angle, frequency, length,
    /// time, with the value in the given unit of that dimension.
    pub basis_kind: u8,
    pub basis_value: f64,
    pub basis_unit: u8,
    /// The length resolution context as an opaque pointer (its type lives in
    /// the computed-values header), or null.
    pub length_resolution_context: *const std::ffi::c_void,
    pub callback_context: *mut std::ffi::c_void,
    /// Resolves a non-math function value to a calculation subtree, or null.
    pub resolve_non_math_function:
        unsafe extern "C" fn(context: *mut std::ffi::c_void, shell: *const std::ffi::c_void) -> *const CalcNode,
    /// Looks up a relative-color channel value.
    pub resolve_channel_keyword:
        unsafe extern "C" fn(context: *mut std::ffi::c_void, channel: u8, out_value: *mut f64) -> bool,
    /// Produces the random base value for a random() node's sharing options,
    /// or reports that the context cannot (it needs per-element state).
    pub random_base_value: unsafe extern "C" fn(
        context: *mut std::ffi::c_void,
        sharing: *const std::ffi::c_void,
        out_value: *mut f64,
    ) -> bool,
    /// Absolutizes a random() node's sharing options at computed-value time,
    /// returning a leaked strong reference to the fixed value, or null.
    pub absolutize_random_sharing: unsafe extern "C" fn(
        context: *mut std::ffi::c_void,
        sharing: *const std::ffi::c_void,
    ) -> *const std::ffi::c_void,
    /// Resolves a length the Rust resolver cannot (container-relative units,
    /// which need the per-element query container lookup).
    pub resolve_length:
        unsafe extern "C" fn(context: *mut std::ffi::c_void, value: f64, unit: u8, out_px: *mut f64) -> bool,
}

/// The resolve-as target from a calculated value's creation-time fields.
fn resolve_as_from_fields(has_percentages_resolve_as: bool, is_number: bool, base: u8) -> Option<ResolveAs> {
    if !has_percentages_resolve_as {
        None
    } else if is_number {
        Some(ResolveAs::Number)
    } else {
        Some(ResolveAs::Base(base))
    }
}

/// The type a percentage leaf takes under the given resolve-as target.
fn percentage_leaf_type_for(resolve_as: Option<ResolveAs>) -> CalcNumericType {
    let mut percentage_leaf_type = CalcNumericType::default();
    match resolve_as {
        Some(ResolveAs::Base(base)) => {
            percentage_leaf_type.exponents[base as usize] = Some(1);
            percentage_leaf_type.percent_hint = Some(base);
        }
        _ => percentage_leaf_type.exponents[BASE_TYPE_PERCENT] = Some(1),
    }
    percentage_leaf_type
}

/// Builds the evaluation context and simplify callbacks from an FFI resolution
/// context and runs `f` with them; the setup shared by resolution and
/// absolutization.
fn with_ffi_evaluation<R>(
    resolve_as: Option<ResolveAs>,
    context: &FfiCalcResolutionContext,
    f: impl FnOnce(&CalcEvaluationContext, &CalcSimplifyCallbacks) -> R,
) -> R {
    let percentage_leaf_type = percentage_leaf_type_for(resolve_as);
    let percentage_basis = match context.basis_kind {
        0 => None,
        1 => Some(CalcNumericValue::Angle {
            value: context.basis_value,
            unit: context.basis_unit,
        }),
        2 => Some(CalcNumericValue::Frequency {
            value: context.basis_value,
            unit: context.basis_unit,
        }),
        3 => Some(CalcNumericValue::Length {
            value: context.basis_value,
            unit: context.basis_unit,
        }),
        4 => Some(CalcNumericValue::Time {
            value: context.basis_value,
            unit: context.basis_unit,
        }),
        _ => unreachable!("invalid percentage basis kind"),
    };
    let random_base_value = |sharing: &RetainedStyleValue| -> Option<f64> {
        let mut value = 0.0;
        if unsafe { (context.random_base_value)(context.callback_context, sharing.shell_pointer(), &raw mut value) } {
            Some(value)
        } else {
            None
        }
    };
    let resolve_length_fallback = |value: f64, unit: u8| -> Option<f64> {
        let mut px = 0.0;
        if unsafe { (context.resolve_length)(context.callback_context, value, unit, &raw mut px) } {
            Some(px)
        } else {
            None
        }
    };
    let evaluation_context = CalcEvaluationContext {
        percentage_leaf_type: &percentage_leaf_type,
        resolve_as,
        percentage_basis,
        length_resolution: LengthResolution {
            context: if context.length_resolution_context.is_null() {
                None
            } else {
                Some(unsafe {
                    &*(context.length_resolution_context as *const crate::style_compute::FfiLengthResolutionContext)
                })
            },
            fallback: Some(&resolve_length_fallback),
        },
        random_base_value: Some(&random_base_value),
    };
    let absolutize_random_sharing = |sharing: &RetainedStyleValue| -> Option<RetainedStyleValue> {
        let absolutized =
            unsafe { (context.absolutize_random_sharing)(context.callback_context, sharing.shell_pointer()) };
        if absolutized.is_null() {
            None
        } else {
            Some(unsafe { RetainedStyleValue::from_shell_pointer(absolutized) })
        }
    };
    let callbacks = CalcSimplifyCallbacks {
        absolutize_random_sharing: &absolutize_random_sharing,
        resolve_non_math_function: &|value| {
            let resolved =
                unsafe { (context.resolve_non_math_function)(context.callback_context, value.shell_pointer()) };
            if resolved.is_null() {
                None
            } else {
                Some(unsafe { Arc::from_raw(resolved) })
            }
        },
        resolve_channel_keyword: &|channel| {
            let mut value = 0.0;
            if unsafe { (context.resolve_channel_keyword)(context.callback_context, channel, &raw mut value) } {
                Some(value)
            } else {
                None
            }
        },
    };
    f(&evaluation_context, &callbacks)
}

/// The outcome of resolving a calculation.
#[repr(C)]
pub struct FfiResolvedCalc {
    /// Whether the calculation fully resolved to a numeric value.
    pub resolved: bool,
    pub value: f64,
    pub numeric_type: FfiNumericType,
}

/// The post-simplification resolution tail shared by the FFI entry and the
/// crate-internal resolver: materialize the numeric result and apply the
/// css-values-4 top-level censoring and clamping rules.
fn resolve_simplified_calculation(
    simplified: &Arc<CalcNode>,
    evaluation_context: &CalcEvaluationContext,
    resolve_as: Option<ResolveAs>,
    resolve_numbers_as_integers: bool,
    accepted_ranges: &[crate::style_value::RetainedNumericRangeByType],
    apply_censoring_and_clamping: bool,
) -> Option<(f64, Option<CalcNumericType>)> {
    if !matches!(&**simplified, CalcNode::Numeric(..)) || (simplified.contains_percentage() && resolve_as.is_some()) {
        return None;
    }
    let result = simplified.try_canonical_result(evaluation_context)?;

    let mut raw_value = result.value;
    if apply_censoring_and_clamping {
        // https://drafts.csswg.org/css-values/#calc-ieee
        // NaN does not escape a top-level calculation; it's censored into a zero value.
        if raw_value.is_nan() {
            raw_value = 0.0;
        }

        // https://drafts.csswg.org/css-values/#calc-range
        // The value resulting from a top-level calculation must be clamped to the range
        // allowed in the target context.
        let numeric_type = result.numeric_type.as_ref().expect("canonical result has a type");
        let wanted_value_type = if numeric_type.matches_number(resolve_as) {
            Some(if resolve_numbers_as_integers {
                value_type::INTEGER
            } else {
                value_type::NUMBER
            })
        } else if numeric_type.matches_dimension(1, resolve_as) {
            Some(value_type::ANGLE)
        } else if numeric_type.matches_dimension(5, resolve_as) {
            Some(value_type::FLEX)
        } else if numeric_type.matches_dimension(3, resolve_as) {
            Some(value_type::FREQUENCY)
        } else if numeric_type.matches_dimension(0, resolve_as) {
            Some(value_type::LENGTH)
        } else if numeric_type.matches_percentage() {
            Some(value_type::PERCENTAGE)
        } else if numeric_type.matches_dimension(4, resolve_as) {
            Some(value_type::RESOLUTION)
        } else if numeric_type.matches_dimension(2, resolve_as) {
            Some(value_type::TIME)
        } else {
            None
        };
        let range = wanted_value_type.and_then(|wanted| {
            accepted_ranges
                .iter()
                .find(|entry| entry.value_type() == wanted)
                .map(|entry| entry.range())
        });
        // FIXME: Infinity for integers should be i32 max rather than float max.
        let (min, max) = range.unwrap_or((f32::MIN as f64, f32::MAX as f64));
        raw_value = raw_value.clamp(min, max);
    }
    Some((raw_value, result.numeric_type))
}

/// Resolves a calculated value with no external context: the equivalent of a
/// resolution against an empty C++ resolution context, where the callbacks for
/// non-math functions, relative-color channels, and random() all fail. Used by
/// the style computation core's own property helpers.
fn resolve_calculated_without_context(
    calculated: &crate::style_value::StyleValueData,
    percentage_basis: Option<CalcNumericValue>,
) -> Option<(f64, CalcNumericType, Option<ResolveAs>)> {
    use crate::style_value::StyleValueData;
    let StyleValueData::Calculated {
        rust_calculation,
        has_percentages_resolve_as,
        resolve_as_is_number,
        resolve_as_base,
        resolve_numbers_as_integers,
        accepted_ranges,
        ..
    } = calculated
    else {
        return None;
    };
    let root = rust_calculation.node_arc();
    let resolve_as = resolve_as_from_fields(*has_percentages_resolve_as, *resolve_as_is_number, *resolve_as_base);
    let percentage_leaf_type = percentage_leaf_type_for(resolve_as);
    let evaluation_context = CalcEvaluationContext {
        percentage_leaf_type: &percentage_leaf_type,
        resolve_as,
        percentage_basis,
        length_resolution: LengthResolution::default(),
        random_base_value: None,
    };
    let callbacks = CalcSimplifyCallbacks {
        absolutize_random_sharing: &|_| None,
        resolve_non_math_function: &|_| None,
        resolve_channel_keyword: &|_| None,
    };
    let simplified = root.simplify(&evaluation_context, &callbacks);
    let (value, numeric_type) = resolve_simplified_calculation(
        &simplified,
        &evaluation_context,
        resolve_as,
        *resolve_numbers_as_integers,
        accepted_ranges.as_slice(),
        true,
    )?;
    Some((value, numeric_type.expect("canonical result has a type"), resolve_as))
}

/// Resolves a calculated value that must produce a number, with no external
/// context; the equivalent of the C++ resolve_number with an empty context.
pub(crate) fn resolve_calculated_number_without_context(
    calculated: &crate::style_value::StyleValueData,
) -> Option<f64> {
    let (value, numeric_type, resolve_as) = resolve_calculated_without_context(calculated, None)?;
    numeric_type.matches_number(resolve_as).then_some(value)
}

/// Resolves a calculated value that must produce a percentage, with no
/// external context; the equivalent of the C++ resolve_percentage with an
/// empty context.
pub(crate) fn resolve_calculated_percentage_without_context(
    calculated: &crate::style_value::StyleValueData,
) -> Option<f64> {
    let (value, numeric_type, ..) = resolve_calculated_without_context(calculated, None)?;
    numeric_type.matches_percentage().then_some(value)
}

/// Resolves a calculated value that must produce a number and rounds it to the
/// nearest integer (toward +inf on a .5 fraction), with no external context;
/// the equivalent of the C++ resolve_integer with an empty context.
pub(crate) fn resolve_calculated_integer_without_context(
    calculated: &crate::style_value::StyleValueData,
) -> Option<i32> {
    let (value, numeric_type, resolve_as) = resolve_calculated_without_context(calculated, None)?;
    if !numeric_type.matches_number(resolve_as) {
        return None;
    }
    // https://drafts.csswg.org/css-values-4/#css-round-to-the-nearest-integer
    // Round toward +inf when the fractional portion is exactly 0.5, matching round_to_nearest_integer.
    if value.is_nan() {
        return Some(0);
    }
    if value.is_infinite() {
        return Some(if value > 0.0 { i32::MAX } else { i32::MIN });
    }
    Some((value + 0.5).floor().clamp(i32::MIN as f64, i32::MAX as f64) as i32)
}

/// Resolves a calculated value that must produce a length, with a percentage
/// basis length; the equivalent of the C++ resolve_length with a percentage
/// basis and an otherwise empty context. Returns pixels.
pub(crate) fn resolve_calculated_length_without_context(
    calculated: &crate::style_value::StyleValueData,
    percentage_basis_px: f64,
) -> Option<f64> {
    let basis = CalcNumericValue::Length {
        value: percentage_basis_px,
        unit: canonical_unit_code(&crate::style_compute::LENGTH_UNIT_CANONICAL_PX_RATIOS),
    };
    let (value, numeric_type, resolve_as) = resolve_calculated_without_context(calculated, Some(basis))?;
    (numeric_type.matches_dimension(0, resolve_as) || numeric_type.matches_percentage()).then_some(value)
}

/// The outcome of resolving a line-height calculation: a pixel length or a
/// unitless number multiplier.
pub(crate) enum ResolvedLineHeightCalc {
    Px(f64),
    Number(f64),
}

/// Resolves a line-height calculation against the computed font size: lengths
/// and percentages resolve to pixels, numbers stay multipliers, as in the C++
/// compute_line_height calc handling.
pub(crate) fn resolve_calculated_line_height_without_context(
    calculated: &crate::style_value::StyleValueData,
    computed_font_size_px: f64,
) -> Option<ResolvedLineHeightCalc> {
    let basis = CalcNumericValue::Length {
        value: computed_font_size_px,
        unit: canonical_unit_code(&crate::style_compute::LENGTH_UNIT_CANONICAL_PX_RATIOS),
    };
    let (value, numeric_type, resolve_as) = resolve_calculated_without_context(calculated, Some(basis))?;
    if numeric_type.matches_dimension(0, resolve_as) || numeric_type.matches_percentage() {
        return Some(ResolvedLineHeightCalc::Px(value));
    }
    numeric_type
        .matches_number(resolve_as)
        .then_some(ResolvedLineHeightCalc::Number(value))
}

/// Resolves a calculated style value: simplifies its tree with used-value
/// information and, when the tree collapses to a numeric leaf, applies the
/// css-values-4 top-level censoring and clamping rules.
///
/// # Safety
/// `calculated` must point at Calculated style value data and `context` at a
/// valid resolution context.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_calc_resolve(
    calculated: *const std::ffi::c_void,
    context: *const FfiCalcResolutionContext,
    apply_censoring_and_clamping: bool,
) -> FfiResolvedCalc {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CalcOperationEntry);
    use crate::style_value::StyleValueData;
    crate::abort_on_panic(|| {
        let StyleValueData::Calculated {
            rust_calculation,
            has_percentages_resolve_as,
            resolve_as_is_number,
            resolve_as_base,
            resolve_numbers_as_integers,
            accepted_ranges,
            ..
        } = (unsafe { &*(calculated as *const StyleValueData) })
        else {
            unreachable!("rust_calc_resolve requires calculated value data");
        };
        let context = unsafe { &*context };
        let root = rust_calculation.node_arc();

        // The percentage leaf type and resolve-as target from the creation-time fields.
        let resolve_as = resolve_as_from_fields(*has_percentages_resolve_as, *resolve_as_is_number, *resolve_as_base);
        with_ffi_evaluation(resolve_as, context, |evaluation_context, callbacks| {
            // The calculation tree is again simplified at used value time.
            let simplified = root.simplify(evaluation_context, callbacks);
            match resolve_simplified_calculation(
                &simplified,
                evaluation_context,
                resolve_as,
                *resolve_numbers_as_integers,
                accepted_ranges.as_slice(),
                apply_censoring_and_clamping,
            ) {
                Some((value, numeric_type)) => FfiResolvedCalc {
                    resolved: true,
                    value,
                    numeric_type: FfiNumericType::from_calc(numeric_type),
                },
                None => FfiResolvedCalc {
                    resolved: false,
                    value: 0.0,
                    numeric_type: FfiNumericType::from_calc(None),
                },
            }
        })
    })
}

/// The output surface for serialization: the structure is built here, but
/// every formatted byte comes from the C++ side, so leaf formatting stays
/// identical to the values' own serializers.
#[repr(C)]
pub struct FfiCalcSerializationCallbacks {
    pub context: *mut std::ffi::c_void,
    pub append_literal: unsafe extern "C" fn(context: *mut std::ffi::c_void, bytes: *const u8, length: usize),
    /// Appends a numeric leaf: kind follows the numeric dimension order, and
    /// the C++ side materializes the value type and runs its serializer.
    pub append_numeric_leaf:
        unsafe extern "C" fn(context: *mut std::ffi::c_void, kind: u8, value: f64, unit: u8, resolved_mode: bool),
    /// Serializes a style value per its own rules, reporting whether any
    /// bytes were appended (random()'s sharing options may serialize empty).
    pub append_style_value:
        unsafe extern "C" fn(context: *mut std::ffi::c_void, shell: *const std::ffi::c_void) -> bool,
    pub append_channel_name: unsafe extern "C" fn(context: *mut std::ffi::c_void, channel: u8),
}

impl CalcNumericValue {
    fn leaf_parts(self) -> (u8, f64, u8) {
        match self {
            CalcNumericValue::Number { value, number_type } => (0, value, number_type),
            CalcNumericValue::Angle { value, unit } => (1, value, unit),
            CalcNumericValue::Flex { value, unit } => (2, value, unit),
            CalcNumericValue::Frequency { value, unit } => (3, value, unit),
            CalcNumericValue::Length { value, unit } => (4, value, unit),
            CalcNumericValue::Percentage(value) => (5, value, 0),
            CalcNumericValue::Resolution { value, unit } => (6, value, unit),
            CalcNumericValue::Time { value, unit } => (7, value, unit),
        }
    }

    fn unit_name(self) -> &'static str {
        match self {
            CalcNumericValue::Number { .. } => "",
            CalcNumericValue::Percentage(..) => "%",
            CalcNumericValue::Angle { unit, .. } => ANGLE_UNIT_NAMES[unit as usize],
            CalcNumericValue::Flex { unit, .. } => FLEX_UNIT_NAMES[unit as usize],
            CalcNumericValue::Frequency { unit, .. } => FREQUENCY_UNIT_NAMES[unit as usize],
            CalcNumericValue::Length { unit, .. } => crate::style_compute::LENGTH_UNIT_NAMES[unit as usize],
            CalcNumericValue::Resolution { unit, .. } => RESOLUTION_UNIT_NAMES[unit as usize],
            CalcNumericValue::Time { unit, .. } => TIME_UNIT_NAMES[unit as usize],
        }
    }
}

struct CalcSerializer<'a> {
    callbacks: &'a FfiCalcSerializationCallbacks,
    resolved_mode: bool,
    resolve_numbers_as_integers: bool,
    accepted_ranges: &'a [crate::style_value::RetainedNumericRangeByType],
}

impl CalcSerializer<'_> {
    fn literal(&self, text: &str) {
        crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CalcSerializationCallback);
        unsafe { (self.callbacks.append_literal)(self.callbacks.context, text.as_ptr(), text.len()) };
    }

    fn leaf(&self, value: CalcNumericValue) {
        let (kind, raw, unit) = value.leaf_parts();
        crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CalcSerializationCallback);
        unsafe { (self.callbacks.append_numeric_leaf)(self.callbacks.context, kind, raw, unit, self.resolved_mode) };
    }

    fn function_name(node: &CalcNode) -> &'static str {
        match node {
            CalcNode::Min(..) => "min",
            CalcNode::Max(..) => "max",
            CalcNode::Clamp { .. } => "clamp",
            CalcNode::Abs(..) => "abs",
            CalcNode::Sign(..) => "sign",
            CalcNode::Sin(..) => "sin",
            CalcNode::Cos(..) => "cos",
            CalcNode::Tan(..) => "tan",
            CalcNode::Asin(..) => "asin",
            CalcNode::Acos(..) => "acos",
            CalcNode::Atan(..) => "atan",
            CalcNode::Atan2 { .. } => "atan2",
            CalcNode::Pow { .. } => "pow",
            CalcNode::Sqrt(..) => "sqrt",
            CalcNode::Hypot(..) => "hypot",
            CalcNode::Log { .. } => "log",
            CalcNode::Exp(..) => "exp",
            CalcNode::Round { .. } => "round",
            CalcNode::Mod { .. } => "mod",
            CalcNode::Rem { .. } => "rem",
            CalcNode::Progress { .. } => "progress",
            _ => unreachable!("node has no function name"),
        }
    }

    /// https://drafts.csswg.org/css-values/#calc-range
    /// The step-1 clamping of a resolved numeric leaf, with NaN censored to zero.
    fn clamp_and_censor(&self, value: CalcNumericValue) -> CalcNumericValue {
        let wanted = match value {
            CalcNumericValue::Number { .. } => {
                if self.resolve_numbers_as_integers {
                    value_type::INTEGER
                } else {
                    value_type::NUMBER
                }
            }
            CalcNumericValue::Angle { .. } => value_type::ANGLE,
            CalcNumericValue::Flex { .. } => value_type::FLEX,
            CalcNumericValue::Frequency { .. } => value_type::FREQUENCY,
            CalcNumericValue::Length { .. } => value_type::LENGTH,
            CalcNumericValue::Percentage(..) => value_type::PERCENTAGE,
            CalcNumericValue::Resolution { .. } => value_type::RESOLUTION,
            CalcNumericValue::Time { .. } => value_type::TIME,
        };
        let range = self
            .accepted_ranges
            .iter()
            .find(|entry| entry.value_type() == wanted)
            .map(|entry| entry.range())
            // FIXME: Min and max values for Integer should be based on i32 rather than float.
            .unwrap_or((f32::MIN as f64, f32::MAX as f64));
        // https://drafts.csswg.org/css-values-4/#css-round-to-the-nearest-integer
        // Numbers round to the nearest integer in integer contexts, in the
        // direction of positive infinity when the fraction is exactly half.
        let raw = if matches!(value, CalcNumericValue::Number { .. }) && self.resolve_numbers_as_integers {
            (value.raw() + 0.5).floor()
        } else {
            value.raw()
        };
        // https://drafts.csswg.org/css-values/#calc-ieee
        // NaN does not escape a top-level calculation; it's censored into a zero value.
        let censored = if raw.is_nan() { 0.0 } else { raw };
        value.with_raw(censored.clamp(range.0, range.1))
    }

    /// https://drafts.csswg.org/css-values-4/#serialize-a-math-function
    fn serialize_math_function(&self, node: &Arc<CalcNode>) {
        // To serialize a math function fn:

        // 1. If the root of the calculation tree fn represents is a numeric value (number,
        //    percentage, or dimension), and the serialization being produced is of a computed
        //    value or later, then clamp the value to the range allowed for its context (if
        //    necessary), then serialize the value as normal and return the result.
        if let CalcNode::Numeric(value) = &**node {
            if self.resolved_mode {
                self.leaf(self.clamp_and_censor(*value));
                return;
            }
            // 2. If fn represents an infinite or NaN value:
            let raw = match value {
                CalcNumericValue::Percentage(percentage) => percentage / 100.0,
                _ => value.raw(),
            };
            if raw.is_nan() || raw.is_infinite() {
                // 1. Let s be the string "calc(".
                self.literal("calc(");
                // 2. Serialize the keyword infinity, -infinity, or NaN, as appropriate to
                //    represent the value, and append it to s.
                self.literal(if raw.is_nan() {
                    "NaN"
                } else if raw > 0.0 {
                    "infinity"
                } else {
                    "-infinity"
                });
                // 3. If fn's type is anything other than «[ ]» (empty, representing a <number>),
                //    append " * " to s. Create a numeric value in the canonical unit for fn's type
                //    (such as px for <length>), with a value of 1. Serialize this numeric value
                //    and append it to s.
                let suffix = match value {
                    CalcNumericValue::Number { .. } => "",
                    CalcNumericValue::Angle { .. } => " * 1deg",
                    CalcNumericValue::Flex { .. } => " * 1fr",
                    CalcNumericValue::Frequency { .. } => " * 1hz",
                    CalcNumericValue::Length { .. } => " * 1px",
                    CalcNumericValue::Percentage(..) => " * 1%",
                    CalcNumericValue::Resolution { .. } => " * 1dppx",
                    CalcNumericValue::Time { .. } => " * 1s",
                };
                self.literal(suffix);
                // 4. Append ")" to s, then return it.
                self.literal(")");
                return;
            }
        }
        // AD-HOC: random() serializes directly since it has abnormal children: the sharing
        // options value, which may serialize empty, and the nullable step.
        if let CalcNode::Random {
            min,
            max,
            step,
            sharing,
            ..
        } = &**node
        {
            self.literal("random(");
            crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CalcSerializationCallback);
            let appended =
                unsafe { (self.callbacks.append_style_value)(self.callbacks.context, sharing.shell_pointer()) };
            if appended {
                self.literal(", ");
            }
            self.serialize_tree(min, true);
            self.literal(", ");
            self.serialize_tree(max, true);
            if let Some(step) = step {
                self.literal(", ");
                self.serialize_tree(step, true);
            }
            self.literal(")");
            return;
        }

        // AD-HOC: A non-math function has no calc() wrapper; serialize it directly.
        if matches!(&**node, CalcNode::NonMathFunction { .. }) {
            self.serialize_tree(node, false);
            return;
        }

        // 3. If the calculation tree's root node is a numeric value, or a calc-operator node,
        //    let s be a string initially containing "calc(".
        //    Otherwise, let s be a string initially containing the name of the root node,
        //    lowercased (such as "sin" or "max"), followed by a "(" (open parenthesis).
        let is_operator = matches!(
            &**node,
            CalcNode::Sum(..) | CalcNode::Product(..) | CalcNode::Negate(..) | CalcNode::Invert(..)
        );

        // 4. For each child of the root node, serialize the calculation tree.
        //    If a result of this serialization starts with a "(" (open parenthesis) and ends with
        //    a ")" (close parenthesis), remove those characters from the result.
        //    Concatenate all of the results using ", " (comma followed by space), then append the
        //    result to s.

        // Spec issue: https://github.com/w3c/csswg-drafts/issues/11783
        //             The four AD-HOCs in this step are mentioned there.
        // AD-HOC: Numeric nodes have no children and should serialize directly.
        // AD-HOC: calc-operator nodes should also serialize directly, instead of separating their
        //         children by commas.
        // AD-HOC: ChannelKeyword nodes, used for relative-color syntax, serialize directly as the
        //         keyword name.
        if matches!(&**node, CalcNode::Numeric(..) | CalcNode::ChannelKeyword(..)) || is_operator {
            self.literal("calc(");
            self.serialize_tree(node, false);
            self.literal(")");
            return;
        }

        self.literal(Self::function_name(node));
        self.literal("(");
        let mut first = true;
        // AD-HOC: For round(), the first child is a <rounding-strategy>, which is incompatible
        //         with "serialize a calculation tree". So, we serialize it directly first, and
        //         hope for the best.
        if let CalcNode::Round { strategy, .. } = &**node {
            self.literal(match *strategy {
                0 => "down",
                1 => "nearest",
                2 => "to-zero",
                3 => "up",
                _ => unreachable!("invalid rounding strategy"),
            });
            first = false;
        }
        // AD-HOC: For progress() we serialize 'no-clamp' separately since it's not a calculation
        //         node and shouldn't be followed by a comma.
        if let CalcNode::Progress { no_clamp: true, .. } = &**node {
            self.literal("no-clamp ");
        }
        let mut children = Vec::new();
        node.for_each_child(&mut |child| children.push(child.clone()));
        for child in &children {
            if !first {
                self.literal(", ");
            }
            first = false;
            self.serialize_tree(child, false);
        }
        // 5. Append ")" (close parenthesis) to s.
        self.literal(")");
        // 6. Return s.
    }

    /// https://drafts.csswg.org/css-values-4/#sort-a-calculations-children
    fn sort_children(children: &[Arc<CalcNode>]) -> Vec<Arc<CalcNode>> {
        let mut remaining: Vec<Arc<CalcNode>> = children.to_vec();
        let mut result = Vec::with_capacity(remaining.len());
        // 2. If nodes contains a number, remove it and append it.
        if let Some(index) = remaining
            .iter()
            .position(|node| matches!(&**node, CalcNode::Numeric(CalcNumericValue::Number { .. })))
        {
            result.push(remaining.remove(index));
        }
        // 3. If nodes contains a percentage, remove it and append it.
        if let Some(index) = remaining
            .iter()
            .position(|node| matches!(&**node, CalcNode::Numeric(CalcNumericValue::Percentage(..))))
        {
            result.push(remaining.remove(index));
        }
        // 4. If nodes contains any dimensions, remove them, sort them by their units ordered
        //    ASCII case-insensitively, and append them.
        let mut dimensions = Vec::new();
        let mut index = 0;
        while index < remaining.len() {
            let is_dimension = matches!(
                &*remaining[index],
                CalcNode::Numeric(value) if !matches!(value, CalcNumericValue::Number { .. } | CalcNumericValue::Percentage(..))
            );
            if is_dimension {
                dimensions.push(remaining.remove(index));
            } else {
                index += 1;
            }
        }
        dimensions.sort_by(|a, b| {
            let unit_of = |node: &Arc<CalcNode>| {
                let CalcNode::Numeric(value) = &**node else {
                    unreachable!();
                };
                value.unit_name().to_ascii_lowercase()
            };
            unit_of(a).cmp(&unit_of(b))
        });
        result.extend(dimensions);
        // 5. Append the remaining nodes.
        result.extend(remaining);
        result
    }

    /// https://drafts.csswg.org/css-values-4/#serialize-a-calculation-tree
    fn serialize_tree(&self, node: &Arc<CalcNode>, emit_outer_parentheses: bool) {
        // 1. Let root be the root node of the calculation tree.
        // NOTE: Already the case.
        match &**node {
            // 2. If root is a numeric value, or a non-math function, serialize root per the normal
            //    rules for it and return the result.
            CalcNode::Numeric(value) => self.leaf(*value),
            CalcNode::NonMathFunction { value, .. } => {
                crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CalcSerializationCallback);
                unsafe { (self.callbacks.append_style_value)(self.callbacks.context, value.shell_pointer()) };
            }
            // AD-HOC: ChannelKeyword nodes, used for relative-color syntax, serialize directly as
            //         the keyword name.
            CalcNode::ChannelKeyword(channel) => unsafe {
                crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CalcSerializationCallback);
                (self.callbacks.append_channel_name)(self.callbacks.context, *channel);
            },
            // 4. If root is a Negate node, let s be a string initially containing "(-1 * ".
            CalcNode::Negate(child) => {
                if emit_outer_parentheses {
                    self.literal("(");
                }
                self.literal("-1 * ");
                // Serialize root's child, and append it to s.
                self.serialize_tree(child, true);
                // Append ")" to s, then return it.
                if emit_outer_parentheses {
                    self.literal(")");
                }
            }
            // 5. If root is an Invert node, let s be a string initially containing "(1 / ".
            CalcNode::Invert(child) => {
                if emit_outer_parentheses {
                    self.literal("(");
                }
                self.literal("1 / ");
                // Serialize root's child, and append it to s.
                self.serialize_tree(child, true);
                // Append ")" to s, then return it.
                if emit_outer_parentheses {
                    self.literal(")");
                }
            }
            // 6. If root is a Sum node, let s be a string initially containing "(".
            CalcNode::Sum(children) => {
                if emit_outer_parentheses {
                    self.literal("(");
                }
                let sorted = Self::sort_children(children);
                // Serialize root's first child, and append it to s.
                self.serialize_tree(&sorted[0], true);
                // For each child of root beyond the first:
                for child in &sorted[1..] {
                    match &**child {
                        // 1. If child is a Negate node, append " - " to s, then serialize the
                        //    Negate's child and append the result to s.
                        CalcNode::Negate(negated) => {
                            self.literal(" - ");
                            self.serialize_tree(negated, true);
                        }
                        // 2. If child is a negative numeric value, append " - " to s, then
                        //    serialize the negation of child as normal and append the result to s.
                        CalcNode::Numeric(value) if value.raw() < 0.0 => {
                            self.literal(" - ");
                            self.leaf(value.with_raw(0.0 - value.raw()));
                        }
                        // 3. Otherwise, append " + " to s, then serialize child and append the
                        //    result to s.
                        _ => {
                            self.literal(" + ");
                            self.serialize_tree(child, true);
                        }
                    }
                }
                // Finally, append ")" to s and return it.
                if emit_outer_parentheses {
                    self.literal(")");
                }
            }
            // 7. If root is a Product node, let s be a string initially containing "(".
            CalcNode::Product(children) => {
                if emit_outer_parentheses {
                    self.literal("(");
                }
                let sorted = Self::sort_children(children);
                // Serialize root's first child, and append it to s.
                self.serialize_tree(&sorted[0], true);
                // For each child of root beyond the first:
                for child in &sorted[1..] {
                    match &**child {
                        // 1. If child is an Invert node, append " / " to s, then serialize the
                        //    Invert's child and append the result to s.
                        CalcNode::Invert(inverted) => {
                            self.literal(" / ");
                            self.serialize_tree(inverted, true);
                        }
                        // 2. Otherwise, append " * " to s, then serialize child and append the
                        //    result to s.
                        _ => {
                            self.literal(" * ");
                            self.serialize_tree(child, true);
                        }
                    }
                }
                // Finally, append ")" to s and return it.
                if emit_outer_parentheses {
                    self.literal(")");
                }
            }
            // 3. If root is anything but a Sum, Negate, Product, or Invert node, serialize a math
            //    function for the function corresponding to the node type, treating the node's
            //    children as the function's comma-separated calculation arguments, and return the
            //    result.
            _ => self.serialize_math_function(node),
        }
    }
}

/// Serializes a calculated style value's math function through the callback
/// surface.
///
/// # Safety
/// `calculated` must point at Calculated style value data and `callbacks` at a
/// valid callback table.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_calc_serialize(
    calculated: *const std::ffi::c_void,
    callbacks: *const FfiCalcSerializationCallbacks,
    resolved_mode: bool,
) {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CalcOperationEntry);
    use crate::style_value::StyleValueData;
    crate::abort_on_panic(|| {
        let StyleValueData::Calculated {
            rust_calculation,
            resolve_numbers_as_integers,
            accepted_ranges,
            ..
        } = (unsafe { &*(calculated as *const StyleValueData) })
        else {
            unreachable!("rust_calc_serialize requires calculated value data");
        };
        let root = rust_calculation.node_arc();
        let serializer = CalcSerializer {
            callbacks: unsafe { &*callbacks },
            resolved_mode,
            resolve_numbers_as_integers: *resolve_numbers_as_integers,
            accepted_ranges: accepted_ranges.as_slice(),
        };
        serializer.serialize_math_function(&root);
    });
}

impl CalcNode {
    /// Structural equality over two calculation trees, mirroring the C++ node
    /// equals implementations: kinds, leaf values and child structures must
    /// match, with the style values carried by random() and non-math-function
    /// nodes compared through the given value-equality callback.
    pub(crate) fn structurally_equals(
        &self,
        other: &CalcNode,
        style_value_equals: &dyn Fn(&RetainedStyleValue, &RetainedStyleValue) -> bool,
    ) -> bool {
        let children_equal = |a: &[Arc<CalcNode>], b: &[Arc<CalcNode>]| {
            a.len() == b.len()
                && a.iter()
                    .zip(b.iter())
                    .all(|(a, b)| a.structurally_equals(b, style_value_equals))
        };
        match (self, other) {
            (CalcNode::Numeric(a), CalcNode::Numeric(b)) => a == b,
            (CalcNode::ChannelKeyword(a), CalcNode::ChannelKeyword(b)) => a == b,
            (CalcNode::Sum(a), CalcNode::Sum(b))
            | (CalcNode::Product(a), CalcNode::Product(b))
            | (CalcNode::Min(a), CalcNode::Min(b))
            | (CalcNode::Max(a), CalcNode::Max(b))
            | (CalcNode::Hypot(a), CalcNode::Hypot(b)) => children_equal(a, b),
            (CalcNode::Negate(a), CalcNode::Negate(b))
            | (CalcNode::Invert(a), CalcNode::Invert(b))
            | (CalcNode::Abs(a), CalcNode::Abs(b))
            | (CalcNode::Sign(a), CalcNode::Sign(b))
            | (CalcNode::Sin(a), CalcNode::Sin(b))
            | (CalcNode::Cos(a), CalcNode::Cos(b))
            | (CalcNode::Tan(a), CalcNode::Tan(b))
            | (CalcNode::Asin(a), CalcNode::Asin(b))
            | (CalcNode::Acos(a), CalcNode::Acos(b))
            | (CalcNode::Atan(a), CalcNode::Atan(b))
            | (CalcNode::Sqrt(a), CalcNode::Sqrt(b))
            | (CalcNode::Exp(a), CalcNode::Exp(b)) => a.structurally_equals(b, style_value_equals),
            (
                CalcNode::Clamp {
                    min: a_min,
                    center: a_center,
                    max: a_max,
                },
                CalcNode::Clamp {
                    min: b_min,
                    center: b_center,
                    max: b_max,
                },
            ) => {
                a_min.structurally_equals(b_min, style_value_equals)
                    && a_center.structurally_equals(b_center, style_value_equals)
                    && a_max.structurally_equals(b_max, style_value_equals)
            }
            (
                CalcNode::Progress {
                    no_clamp: a_no_clamp,
                    progress: a_progress,
                    from: a_from,
                    to: a_to,
                },
                CalcNode::Progress {
                    no_clamp: b_no_clamp,
                    progress: b_progress,
                    from: b_from,
                    to: b_to,
                },
            ) => {
                a_no_clamp == b_no_clamp
                    && a_progress.structurally_equals(b_progress, style_value_equals)
                    && a_from.structurally_equals(b_from, style_value_equals)
                    && a_to.structurally_equals(b_to, style_value_equals)
            }
            (CalcNode::Atan2 { y: a_y, x: a_x }, CalcNode::Atan2 { y: b_y, x: b_x }) => {
                a_y.structurally_equals(b_y, style_value_equals) && a_x.structurally_equals(b_x, style_value_equals)
            }
            (
                CalcNode::Pow {
                    base: a_base,
                    exponent: a_exponent,
                },
                CalcNode::Pow {
                    base: b_base,
                    exponent: b_exponent,
                },
            ) => {
                a_base.structurally_equals(b_base, style_value_equals)
                    && a_exponent.structurally_equals(b_exponent, style_value_equals)
            }
            (
                CalcNode::Log {
                    value: a_value,
                    base: a_base,
                },
                CalcNode::Log {
                    value: b_value,
                    base: b_base,
                },
            ) => {
                a_value.structurally_equals(b_value, style_value_equals)
                    && a_base.structurally_equals(b_base, style_value_equals)
            }
            (
                CalcNode::Round {
                    strategy: a_strategy,
                    value: a_value,
                    interval: a_interval,
                },
                CalcNode::Round {
                    strategy: b_strategy,
                    value: b_value,
                    interval: b_interval,
                },
            ) => {
                a_strategy == b_strategy
                    && a_value.structurally_equals(b_value, style_value_equals)
                    && a_interval.structurally_equals(b_interval, style_value_equals)
            }
            (
                CalcNode::Mod {
                    value: a_value,
                    modulus: a_modulus,
                },
                CalcNode::Mod {
                    value: b_value,
                    modulus: b_modulus,
                },
            ) => {
                a_value.structurally_equals(b_value, style_value_equals)
                    && a_modulus.structurally_equals(b_modulus, style_value_equals)
            }
            (
                CalcNode::Rem {
                    value: a_value,
                    divisor: a_divisor,
                },
                CalcNode::Rem {
                    value: b_value,
                    divisor: b_divisor,
                },
            ) => {
                a_value.structurally_equals(b_value, style_value_equals)
                    && a_divisor.structurally_equals(b_divisor, style_value_equals)
            }
            (
                CalcNode::Random {
                    min: a_min,
                    max: a_max,
                    step: a_step,
                    sharing: a_sharing,
                },
                CalcNode::Random {
                    min: b_min,
                    max: b_max,
                    step: b_step,
                    sharing: b_sharing,
                },
            ) => {
                a_min.structurally_equals(b_min, style_value_equals)
                    && a_max.structurally_equals(b_max, style_value_equals)
                    && match (a_step, b_step) {
                        (None, None) => true,
                        (Some(a), Some(b)) => a.structurally_equals(b, style_value_equals),
                        _ => false,
                    }
                    && style_value_equals(a_sharing, b_sharing)
            }
            (CalcNode::NonMathFunction { value: a_value, .. }, CalcNode::NonMathFunction { value: b_value, .. }) => {
                style_value_equals(a_value, b_value)
            }
            _ => false,
        }
    }
}

/// Structural equality of two calculated style values' trees.
///
/// # Safety
/// Both pointers must reference Calculated style value data, and the callback
/// must be valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_calc_equals(
    first: *const std::ffi::c_void,
    second: *const std::ffi::c_void,
    context: *mut std::ffi::c_void,
    style_value_equals: unsafe extern "C" fn(
        context: *mut std::ffi::c_void,
        a: *const std::ffi::c_void,
        b: *const std::ffi::c_void,
    ) -> bool,
) -> bool {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CalcOperationEntry);
    use crate::style_value::StyleValueData;
    crate::abort_on_panic(|| {
        let tree_of = |data: *const std::ffi::c_void| {
            let StyleValueData::Calculated { rust_calculation, .. } = (unsafe { &*(data as *const StyleValueData) })
            else {
                unreachable!("rust_calc_equals requires calculated value data");
            };
            rust_calculation.node_arc()
        };
        let first_tree = tree_of(first);
        let second_tree = tree_of(second);
        first_tree.structurally_equals(&second_tree, &|a, b| unsafe {
            style_value_equals(context, a.shell_pointer(), b.shell_pointer())
        })
    })
}

/// The node kind codes exposed to the C++ read API, in a stable documented
/// order for the reification walk.
fn node_kind_code(node: &CalcNode) -> u8 {
    match node {
        CalcNode::Numeric(..) => 0,
        CalcNode::ChannelKeyword(..) => 1,
        CalcNode::Sum(..) => 2,
        CalcNode::Product(..) => 3,
        CalcNode::Negate(..) => 4,
        CalcNode::Invert(..) => 5,
        CalcNode::Min(..) => 6,
        CalcNode::Max(..) => 7,
        CalcNode::Clamp { .. } => 8,
        CalcNode::Progress { .. } => 9,
        CalcNode::Abs(..) => 10,
        CalcNode::Sign(..) => 11,
        CalcNode::Sin(..) => 12,
        CalcNode::Cos(..) => 13,
        CalcNode::Tan(..) => 14,
        CalcNode::Asin(..) => 15,
        CalcNode::Acos(..) => 16,
        CalcNode::Atan(..) => 17,
        CalcNode::Atan2 { .. } => 18,
        CalcNode::Pow { .. } => 19,
        CalcNode::Sqrt(..) => 20,
        CalcNode::Hypot(..) => 21,
        CalcNode::Log { .. } => 22,
        CalcNode::Exp(..) => 23,
        CalcNode::Round { .. } => 24,
        CalcNode::Mod { .. } => 25,
        CalcNode::Rem { .. } => 26,
        CalcNode::Random { .. } => 27,
        CalcNode::NonMathFunction { .. } => 28,
    }
}

/// # Safety
/// `node` must be a valid calculation node pointer for all read functions.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_calc_node_kind(node: *const CalcNode) -> u8 {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CalcNodeQueryEntry);
    crate::abort_on_panic(|| node_kind_code(unsafe { &*node }))
}

/// The node's calculation children in their canonical order. Returned
/// pointers are borrowed from the node's own references.
///
/// # Safety
/// `out_children` must have room for `capacity` entries.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_calc_node_children(
    node: *const CalcNode,
    out_children: *mut *const CalcNode,
    capacity: usize,
) -> usize {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CalcNodeQueryEntry);
    crate::abort_on_panic(|| {
        let mut count = 0;
        unsafe { &*node }.for_each_child(&mut |child| {
            if count < capacity {
                unsafe { *out_children.add(count) = Arc::as_ptr(child) };
            }
            count += 1;
        });
        count
    })
}

/// Reads a numeric leaf's parts; false for non-leaf nodes.
///
/// # Safety
/// All out-pointers must be valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_calc_node_numeric_leaf(
    node: *const CalcNode,
    out_kind: *mut u8,
    out_value: *mut f64,
    out_unit: *mut u8,
) -> bool {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CalcNodeQueryEntry);
    crate::abort_on_panic(|| {
        let CalcNode::Numeric(value) = (unsafe { &*node }) else {
            return false;
        };
        let (kind, raw, unit) = value.leaf_parts();
        unsafe {
            *out_kind = kind;
            *out_value = raw;
            *out_unit = unit;
        }
        true
    })
}

/// The style value carried by a random() or non-math-function node
/// (the sharing options or the function value), or null.
///
/// # Safety
/// `node` must be a valid calculation node pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_calc_node_style_value(node: *const CalcNode) -> *const std::ffi::c_void {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CalcNodeQueryEntry);
    crate::abort_on_panic(|| match unsafe { &*node } {
        CalcNode::Random { sharing, .. } => sharing.shell_pointer(),
        CalcNode::NonMathFunction { value, .. } => value.shell_pointer(),
        _ => std::ptr::null(),
    })
}

/// The numeric type of a node within a calculated value's context, for the
/// reification of math function objects.
///
/// # Safety
/// `calculated` must point at Calculated style value data owning `node`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_calc_node_numeric_type(
    calculated: *const std::ffi::c_void,
    node: *const CalcNode,
) -> FfiNumericType {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CalcNodeQueryEntry);
    use crate::style_value::StyleValueData;
    crate::abort_on_panic(|| {
        let StyleValueData::Calculated {
            has_percentages_resolve_as,
            resolve_as_is_number,
            resolve_as_base,
            ..
        } = (unsafe { &*(calculated as *const StyleValueData) })
        else {
            unreachable!("rust_calc_node_numeric_type requires calculated value data");
        };
        let mut percentage_leaf_type = CalcNumericType::default();
        if *has_percentages_resolve_as && !*resolve_as_is_number {
            percentage_leaf_type.exponents[*resolve_as_base as usize] = Some(1);
            percentage_leaf_type.percent_hint = Some(*resolve_as_base);
        } else {
            percentage_leaf_type.exponents[BASE_TYPE_PERCENT] = Some(1);
        }
        FfiNumericType::from_calc(unsafe { &*node }.numeric_type(&percentage_leaf_type))
    })
}

/// Simplifies a free-standing calculation tree: the css-values-4 algorithm
/// over a borrowed root, returning the simplified tree as a transferred
/// handle. This backs the C++ simplify_a_calculation_tree entry, whose
/// callers simplify trees that are not (yet) owned by a calculated style
/// value: parse-time math functions, interpolation sums, flipped edge
/// offsets, and anchor fallbacks.
///
/// # Safety
/// `root` must be a valid calculation node pointer and `context` a valid
/// resolution context.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_calc_simplify_tree(
    root: *const CalcNode,
    context: *const FfiCalcResolutionContext,
    has_percentages_resolve_as: bool,
    resolve_as_is_number: bool,
    resolve_as_base: u8,
) -> *const CalcNode {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CalcOperationEntry);
    crate::abort_on_panic(|| {
        let context = unsafe { &*context };
        unsafe { Arc::increment_strong_count(root) };
        let root = unsafe { Arc::from_raw(root) };
        let resolve_as = resolve_as_from_fields(has_percentages_resolve_as, resolve_as_is_number, resolve_as_base);
        with_ffi_evaluation(resolve_as, context, |evaluation_context, callbacks| {
            Arc::into_raw(root.simplify(evaluation_context, callbacks))
        })
    })
}

/// The outcome of absolutizing a calculation: either a computed percentage
/// (the percentage-dimension mix special case) or the simplified tree as a
/// transferred handle.
#[repr(C)]
pub struct FfiAbsolutizedCalc {
    pub is_percentage: bool,
    pub percentage_value: f64,
    pub simplified: *const CalcNode,
}

/// Absolutizes a calculated value: simplifies its tree with computed-value
/// information and detects the css-values-4 percentage-dimension mix whose
/// dimension component is zero, which computes to a plain percentage.
///
/// # Safety
/// `calculated` must point at Calculated style value data and `context` at a
/// valid resolution context.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_calc_absolutize(
    calculated: *const std::ffi::c_void,
    context: *const FfiCalcResolutionContext,
) -> FfiAbsolutizedCalc {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CalcOperationEntry);
    use crate::style_value::StyleValueData;
    crate::abort_on_panic(|| {
        let StyleValueData::Calculated {
            rust_calculation,
            has_percentages_resolve_as,
            resolve_as_is_number,
            resolve_as_base,
            ..
        } = (unsafe { &*(calculated as *const StyleValueData) })
        else {
            unreachable!("rust_calc_absolutize requires calculated value data");
        };
        let context = unsafe { &*context };
        let root = rust_calculation.node_arc();

        let resolve_as = resolve_as_from_fields(*has_percentages_resolve_as, *resolve_as_is_number, *resolve_as_base);
        with_ffi_evaluation(resolve_as, context, |evaluation_context, callbacks| {
            let simplified = root.simplify(evaluation_context, callbacks);

            // NOTE: A percentage dimension mix is a Sum with two numeric children of matching base
            //       types, only the first of which has a percent hint.
            // https://drafts.csswg.org/css-values-4/#combine-mixed
            // The computed value of a percentage-dimension mix is a computed percentage if the
            // dimension component is zero.
            if let CalcNode::Sum(children) = &*simplified
                && children.len() == 2
                && let (CalcNode::Numeric(first), CalcNode::Numeric(second)) = (&*children[0], &*children[1])
                && let Some(first_type) = children[0].numeric_type(evaluation_context.percentage_leaf_type)
                && let Some(second_type) = children[1].numeric_type(evaluation_context.percentage_leaf_type)
            {
                let first_base = first_type.entry_with_value_1_while_all_others_are_0();
                let second_base = second_type.entry_with_value_1_while_all_others_are_0();
                if first_base.is_some()
                    && first_base == second_base
                    && first_type.percent_hint.is_some()
                    && second_type.percent_hint.is_none()
                    && is_canonical_unit(*second)
                    && second.to_canonical_number(evaluation_context.length_resolution) == 0.0
                    && let CalcNumericValue::Percentage(percentage) = first
                {
                    return FfiAbsolutizedCalc {
                        is_percentage: true,
                        percentage_value: *percentage,
                        simplified: std::ptr::null(),
                    };
                }
            }

            FfiAbsolutizedCalc {
                is_percentage: false,
                percentage_value: 0.0,
                simplified: Arc::into_raw(simplified),
            }
        })
    })
}
