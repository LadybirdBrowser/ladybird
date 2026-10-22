/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The geometry interfaces, https://drafts.fxtf.org/geometry/

pub(crate) mod dictionaries;
#[cfg(not(test))]
pub(crate) mod dom_matrix;
#[cfg(not(test))]
pub(crate) mod dom_matrix_read_only;
#[cfg(not(test))]
pub(crate) mod dom_point;
#[cfg(not(test))]
pub(crate) mod dom_point_read_only;
#[cfg(not(test))]
pub(crate) mod dom_quad;
#[cfg(not(test))]
pub(crate) mod dom_rect;
#[cfg(not(test))]
pub(crate) mod dom_rect_list;
#[cfg(not(test))]
pub(crate) mod dom_rect_read_only;
#[cfg(not(test))]
mod exports;
pub(crate) mod matrix;

/// https://drafts.fxtf.org/geometry/#nan-safe-minimum
pub(crate) fn nan_safe_minimum(values: &[f64]) -> f64 {
    // The NaN-safe minimum of a non-empty list of unrestricted double values is NaN if any member of the list is NaN,
    // or the minimum of the list otherwise.
    if values.iter().any(|value| value.is_nan()) {
        return f64::NAN;
    }
    // NB: The minimum of equal values is the one first in the list, as AK::min() gives it to C++, so that the sign of
    //     a zero does not change.
    let (last, rest) = values.split_last().expect("the list is not empty");
    rest.iter()
        .rev()
        .fold(*last, |minimum, value| if minimum < *value { minimum } else { *value })
}

/// https://drafts.fxtf.org/geometry/#nan-safe-maximum
pub(crate) fn nan_safe_maximum(values: &[f64]) -> f64 {
    // Analogously, the NaN-safe maximum of a non-empty list of unrestricted double values is NaN if any member of the
    // list is NaN, or the maximum of the list otherwise.
    if values.iter().any(|value| value.is_nan()) {
        return f64::NAN;
    }
    let (last, rest) = values.split_last().expect("the list is not empty");
    rest.iter()
        .rev()
        .fold(*last, |maximum, value| if *value < maximum { maximum } else { *value })
}
