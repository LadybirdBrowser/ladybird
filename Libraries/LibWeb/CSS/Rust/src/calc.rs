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

/// https://drafts.css-houdini.org/css-typed-om-1/#numeric-typing
/// The type of a calculation: an exponent per base type plus the percent
/// hint, mirroring the C++ NumericType.
#[derive(Clone, Copy, PartialEq, Eq, Default)]
#[allow(dead_code)]
pub(crate) struct CalcNumericType {
    /// Exponents indexed by base type: length, angle, time, frequency,
    /// resolution, flex, percent, in the C++ BaseType order.
    pub exponents: [i32; 7],
    /// The percent hint: an index into the base types, when set.
    pub percent_hint: Option<u8>,
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
    /// its retained style value.
    NonMathFunction(RetainedStyleValue),
}

#[allow(dead_code)]
impl CalcNode {
    /// The node's children, for the traversals that do not care about the
    /// node kind.
    pub(crate) fn for_each_child(&self, f: &mut impl FnMut(&Arc<CalcNode>)) {
        match self {
            CalcNode::Numeric(..) | CalcNode::ChannelKeyword(..) | CalcNode::NonMathFunction(..) => {}
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
        assert_eq!(numeric_type.exponents, [0; 7]);
        assert!(numeric_type.percent_hint.is_none());
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
pub unsafe extern "C" fn rust_calc_node_create_non_math_function(value: *const std::ffi::c_void) -> *const CalcNode {
    crate::abort_on_panic(|| {
        handle(CalcNode::NonMathFunction(unsafe {
            RetainedStyleValue::from_shell_pointer(value)
        }))
    })
}

/// # Safety
/// `node` must be a valid transferred handle; this releases it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_calc_node_release(node: *const CalcNode) {
    crate::abort_on_panic(|| drop(unsafe { Arc::from_raw(node) }));
}
