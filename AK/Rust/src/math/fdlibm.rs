/*
 * Copyright 2016 the V8 project authors. All rights reserved.
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: SunPro
 */
// Source: v8/src/base/ieee754.cc, which is adapted from fdlibm (http://www.netlib.org/fdlibm).
//
// ====================================================
// Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
//
// Developed at SunSoft, a Sun Microsystems, Inc. business.
// Permission to use, copy, modify, and distribute this
// software is freely granted, provided that this notice
// is preserved.
// ====================================================

//! The inverse hyperbolic functions for f64, which LLVM libc does not have yet. These are built on its log and log1p.

use super::{log, log1p};
use core::f64::consts::LN_2;

fn high_word(x: f64) -> i32 {
    (x.to_bits() >> 32) as i32
}

/// acosh(x) = log(x + sqrt(x * x - 1)), computed as:
///  - log(x) + ln2, if x is large;
///  - log(2x - 1 / (sqrt(x * x - 1) + x)), if x > 2;
///  - log1p(t + sqrt(2t + t * t)) with t = x - 1, otherwise.
pub fn acosh(x: f64) -> f64 {
    let high = high_word(x);
    // x < 1
    if high < 0x3ff0_0000 {
        return f64::NAN;
    }
    // x > 2^28
    if high >= 0x41b0_0000 {
        // x is Infinity or NaN
        if high >= 0x7ff0_0000 {
            return x + x;
        }
        return log(x) + LN_2;
    }
    if x == 1.0 {
        return 0.0;
    }
    // 2^28 > x > 2
    if high > 0x4000_0000 {
        let t = x * x;
        return log(2.0 * x - 1.0 / (x + (t - 1.0).sqrt()));
    }
    // 1 < x <= 2
    let t = x - 1.0;
    log1p(t + (2.0 * t + t * t).sqrt())
}

/// asinh(x) = sign(x) * log(|x| + sqrt(x * x + 1)), computed as:
///  - x, if 1 + x * x = 1;
///  - sign(x) * (log(|x|) + ln2), if |x| is large;
///  - sign(x) * log(2|x| + 1 / (|x| + sqrt(x * x + 1))), if |x| > 2;
///  - sign(x) * log1p(|x| + x^2 / (1 + sqrt(1 + x^2))), otherwise.
pub fn asinh(x: f64) -> f64 {
    let high = high_word(x);
    let magnitude_high = high & 0x7fff_ffff;
    // x is Infinity or NaN
    if magnitude_high >= 0x7ff0_0000 {
        return x + x;
    }
    // |x| < 2^-28
    if magnitude_high < 0x3e30_0000 {
        return x;
    }
    let magnitude = x.abs();
    let w = if magnitude_high > 0x41b0_0000 {
        // |x| > 2^28
        log(magnitude) + LN_2
    } else if magnitude_high > 0x4000_0000 {
        // 2^28 >= |x| > 2
        log(2.0 * magnitude + 1.0 / ((x * x + 1.0).sqrt() + magnitude))
    } else {
        // 2 >= |x| >= 2^-28
        let t = x * x;
        log1p(magnitude + t / (1.0 + (1.0 + t).sqrt()))
    };
    if high > 0 { w } else { -w }
}

/// atanh(x) = 0.5 * log((1 + x) / (1 - x)), computed for |x| as:
///  - 0.5 * log1p(2|x| / (1 - |x|)), if |x| >= 0.5;
///  - 0.5 * log1p(2|x| + 2|x| * |x| / (1 - |x|)), otherwise.
pub fn atanh(x: f64) -> f64 {
    let magnitude = x.abs();
    // |x| > 1, or x is NaN
    if magnitude.is_nan() || magnitude > 1.0 {
        return f64::NAN;
    }
    if magnitude == 1.0 {
        return if x > 0.0 { f64::INFINITY } else { f64::NEG_INFINITY };
    }
    // |x| < 2^-28
    if high_word(magnitude) < 0x3e30_0000 {
        return x;
    }
    let t = if magnitude < 0.5 {
        let twice = magnitude + magnitude;
        0.5 * log1p(twice + twice * magnitude / (1.0 - magnitude))
    } else {
        0.5 * log1p((magnitude + magnitude) / (1.0 - magnitude))
    };
    if x >= 0.0 { t } else { -t }
}
