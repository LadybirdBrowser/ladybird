/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! QueuingStrategy, https://streams.spec.whatwg.org/#qs-api, and the abstract operations that read it,
//! https://streams.spec.whatwg.org/#qs-abstract-ops

use crate::bindings::callback::CallbackFunction;
use crate::bindings::{Exception, WebIdlResult};
use crate::streams::algorithms::SizeAlgorithm;

/// The dictionary QueuingStrategy of Streams/QueuingStrategy.idl, whose members the bindings convert.
pub(crate) struct QueuingStrategy {
    pub(crate) high_water_mark: Option<f64>,
    pub(crate) size: Option<CallbackFunction>,
}

// https://streams.spec.whatwg.org/#validate-and-normalize-high-water-mark
pub(crate) fn extract_high_water_mark(strategy: &QueuingStrategy, default_hwm: f64) -> WebIdlResult<f64> {
    // 1. If strategy["highWaterMark"] does not exist, return defaultHWM.
    // 2. Let highWaterMark be strategy["highWaterMark"].
    let Some(high_water_mark) = strategy.high_water_mark else {
        return Ok(default_hwm);
    };

    // 3. If highWaterMark is NaN or highWaterMark < 0, throw a RangeError exception.
    if high_water_mark.is_nan() || high_water_mark < 0.0 {
        return Err(Exception::range_error("Invalid value for high water mark"));
    }

    // 4. Return highWaterMark.
    Ok(high_water_mark)
}

// https://streams.spec.whatwg.org/#make-size-algorithm-from-size-function
pub(crate) fn extract_size_algorithm(strategy: &QueuingStrategy) -> SizeAlgorithm {
    match &strategy.size {
        // 1. If strategy["size"] does not exist, return an algorithm that returns 1.
        None => SizeAlgorithm::ReturnsOne,
        // 2. Return an algorithm that performs the following steps, taking a chunk argument:
        //    1. Return the result of invoking strategy["size"] with argument list « chunk ».
        Some(size) => SizeAlgorithm::Strategy(size.clone()),
    }
}
