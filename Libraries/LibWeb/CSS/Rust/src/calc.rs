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
#[allow(dead_code)]
pub enum CalcNumericValue {
    Number(f64),
    Angle { value: f64, unit: u8 },
    Flex { value: f64, unit: u8 },
    Frequency { value: f64, unit: u8 },
    Length { value: f64, unit: u8 },
    Percentage(f64),
    Resolution { value: f64, unit: u8 },
    Time { value: f64, unit: u8 },
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

#[allow(dead_code)]
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
/// units and the numeric type it carries, mirroring the C++ CalculationResult.
#[derive(Clone, Copy, PartialEq, Debug)]
#[allow(dead_code)]
pub(crate) struct CalcResult {
    pub value: f64,
    pub numeric_type: Option<CalcNumericType>,
}

#[allow(dead_code)]
impl CalcResult {
    pub(crate) fn add(&mut self, other: &CalcResult) {
        self.value += other.value;
        self.numeric_type = match (&self.numeric_type, &other.numeric_type) {
            (Some(first), Some(second)) => first.added_to(second),
            _ => None,
        };
    }

    pub(crate) fn subtract(&mut self, other: &CalcResult) {
        self.value -= other.value;
        self.numeric_type = match (&self.numeric_type, &other.numeric_type) {
            (Some(first), Some(second)) => first.added_to(second),
            _ => None,
        };
    }

    pub(crate) fn multiply_by(&mut self, other: &CalcResult) {
        self.value *= other.value;
        self.numeric_type = match (&self.numeric_type, &other.numeric_type) {
            (Some(first), Some(second)) => first.multiplied_by(second),
            _ => None,
        };
    }

    pub(crate) fn divide_by(&mut self, other: &CalcResult) {
        // FIXME: Correctly handle division by zero.
        self.value *= 1.0 / other.value;
        self.numeric_type = match (&self.numeric_type, &other.numeric_type) {
            (Some(first), Some(second)) => first.multiplied_by(&second.inverted()),
            _ => None,
        };
    }

    pub(crate) fn negate(&mut self) {
        self.value = 0.0 - self.value;
    }

    pub(crate) fn invert(&mut self) {
        // FIXME: Correctly handle division by zero.
        self.value = 1.0 / self.value;
        if let Some(numeric_type) = &self.numeric_type {
            self.numeric_type = Some(numeric_type.inverted());
        }
    }
}

#[allow(dead_code)]
impl CalcNumericValue {
    /// The value in its dimension's canonical unit: degrees, fr, hertz,
    /// unrounded pixels, dots per pixel, or seconds. Lengths resolve through
    /// the length resolution context; a relative length without one is NaN,
    /// as in the C++ CalculationResult::from_value.
    pub(crate) fn to_canonical_number(
        self,
        length_resolution_context: Option<&crate::style_compute::FfiLengthResolutionContext>,
    ) -> f64 {
        match self {
            CalcNumericValue::Number(value) => value,
            CalcNumericValue::Percentage(value) => value,
            CalcNumericValue::Angle { value, unit } => value * ANGLE_UNIT_CANONICAL_RATIOS[unit as usize],
            CalcNumericValue::Flex { value, unit } => value * FLEX_UNIT_CANONICAL_RATIOS[unit as usize],
            CalcNumericValue::Frequency { value, unit } => value * FREQUENCY_UNIT_CANONICAL_RATIOS[unit as usize],
            CalcNumericValue::Resolution { value, unit } => value * RESOLUTION_UNIT_CANONICAL_RATIOS[unit as usize],
            CalcNumericValue::Time { value, unit } => value * TIME_UNIT_CANONICAL_RATIOS[unit as usize],
            CalcNumericValue::Length { value, unit } => {
                let Some(context) = length_resolution_context else {
                    return f64::NAN;
                };
                let result = crate::style_compute::absolutize_length_for_calc(value, unit as usize, context);
                if result.handled { result.px } else { f64::NAN }
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
#[allow(dead_code)]
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

#[allow(dead_code)]
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
            CalcNode::Progress { progress, from, to } => {
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
    #[allow(dead_code)]
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
                CalcNumericValue::Number(..) => Some(CalcNumericType::default()),
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
            CalcNode::Progress { progress, from, to } => {
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
        let number = Arc::new(CalcNode::Numeric(CalcNumericValue::Number(2.0)));
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

#[allow(dead_code)]
impl CalcNodeHandle {
    /// # Safety
    /// `raw` must be a handle from one of the construction functions below.
    pub(crate) unsafe fn from_raw(raw: *const CalcNode) -> Self {
        Self { node: raw }
    }

    pub(crate) fn node(&self) -> &CalcNode {
        unsafe { &*self.node }
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

/// Reconstructs the child Arcs from an array of transferred handles.
///
/// # Safety
/// `children` must point at `count` valid transferred handles.
unsafe fn children_from_raw(children: *const *const CalcNode, count: usize) -> Vec<Arc<CalcNode>> {
    (0..count).map(|i| unsafe { Arc::from_raw(*children.add(i)) }).collect()
}

/// # Safety
/// See `children_from_raw`; single-child forms transfer one handle each.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_calc_node_create_numeric_number(value: f64) -> *const CalcNode {
    crate::abort_on_panic(|| handle(CalcNode::Numeric(CalcNumericValue::Number(value))))
}

/// Creates a numeric leaf for a dimension: kind selects the dimension in the
/// order number, angle, flex, frequency, length, percentage, resolution, time.
#[unsafe(no_mangle)]
pub extern "C" fn rust_calc_node_create_numeric_dimension(kind: u8, value: f64, unit: u8) -> *const CalcNode {
    crate::abort_on_panic(|| {
        let numeric = match kind {
            0 => CalcNumericValue::Number(value),
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
    progress: *const CalcNode,
    from: *const CalcNode,
    to: *const CalcNode,
) -> *const CalcNode {
    crate::abort_on_panic(|| {
        handle(CalcNode::Progress {
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
    crate::abort_on_panic(|| drop(unsafe { Arc::from_raw(node) }));
}

/// Whether any leaf of the calculation is a percentage.
///
/// # Safety
/// `node` must be a valid calculation node pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_calc_node_contains_percentage(node: *const CalcNode) -> bool {
    crate::abort_on_panic(|| unsafe { &*node }.contains_percentage())
}
