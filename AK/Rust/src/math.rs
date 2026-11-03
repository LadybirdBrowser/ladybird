/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The math functions of AK/Math.h, which unlike the f32 and f64 methods give the same results on every platform.

mod fdlibm;

pub use fdlibm::{acosh, asinh, atanh};

// SAFETY: These are pure functions defined by AK/MathFunctions.cpp.
unsafe extern "C" {
    safe fn ladybird_math_acos(x: f64) -> f64;
    safe fn ladybird_math_acosf(x: f32) -> f32;
    safe fn ladybird_math_asin(x: f64) -> f64;
    safe fn ladybird_math_asinf(x: f32) -> f32;
    safe fn ladybird_math_atan(x: f64) -> f64;
    safe fn ladybird_math_atanf(x: f32) -> f32;
    safe fn ladybird_math_cbrt(x: f64) -> f64;
    safe fn ladybird_math_cbrtf(x: f32) -> f32;
    safe fn ladybird_math_cos(x: f64) -> f64;
    safe fn ladybird_math_cosf(x: f32) -> f32;
    safe fn ladybird_math_cosh(x: f64) -> f64;
    safe fn ladybird_math_coshf(x: f32) -> f32;
    safe fn ladybird_math_exp(x: f64) -> f64;
    safe fn ladybird_math_expf(x: f32) -> f32;
    safe fn ladybird_math_exp2(x: f64) -> f64;
    safe fn ladybird_math_exp2f(x: f32) -> f32;
    safe fn ladybird_math_expm1(x: f64) -> f64;
    safe fn ladybird_math_expm1f(x: f32) -> f32;
    safe fn ladybird_math_log(x: f64) -> f64;
    safe fn ladybird_math_logf(x: f32) -> f32;
    safe fn ladybird_math_log10(x: f64) -> f64;
    safe fn ladybird_math_log10f(x: f32) -> f32;
    safe fn ladybird_math_log1p(x: f64) -> f64;
    safe fn ladybird_math_log1pf(x: f32) -> f32;
    safe fn ladybird_math_log2(x: f64) -> f64;
    safe fn ladybird_math_log2f(x: f32) -> f32;
    safe fn ladybird_math_sin(x: f64) -> f64;
    safe fn ladybird_math_sinf(x: f32) -> f32;
    safe fn ladybird_math_sinh(x: f64) -> f64;
    safe fn ladybird_math_sinhf(x: f32) -> f32;
    safe fn ladybird_math_tan(x: f64) -> f64;
    safe fn ladybird_math_tanf(x: f32) -> f32;
    safe fn ladybird_math_tanh(x: f64) -> f64;
    safe fn ladybird_math_tanhf(x: f32) -> f32;
    safe fn ladybird_math_atan2(y: f64, x: f64) -> f64;
    safe fn ladybird_math_atan2f(y: f32, x: f32) -> f32;
    safe fn ladybird_math_hypot(x: f64, y: f64) -> f64;
    safe fn ladybird_math_hypotf(x: f32, y: f32) -> f32;
    safe fn ladybird_math_pow(x: f64, y: f64) -> f64;
    safe fn ladybird_math_powf(x: f32, y: f32) -> f32;
}

// SAFETY: These write the sine and cosine through the two pointers, and have no other effects.
unsafe extern "C" {
    fn ladybird_math_sincos(x: f64, sin_value: *mut f64, cos_value: *mut f64);
    fn ladybird_math_sincosf(x: f32, sin_value: *mut f32, cos_value: *mut f32);
}

macro_rules! define_math_functions {
    (
        unary: [$($unary:ident => $unary_f64:ident, $unary_f32:ident;)*]
        binary: [$($binary:ident => $binary_f64:ident, $binary_f32:ident;)*]
    ) => {
        pub trait Float: Copy {
            $(
                #[doc(hidden)]
                fn $unary(x: Self) -> Self;
            )*
            $(
                #[doc(hidden)]
                fn $binary(x: Self, y: Self) -> Self;
            )*
            #[doc(hidden)]
            fn sincos(x: Self) -> (Self, Self);
        }

        impl Float for f64 {
            $(
                #[inline]
                fn $unary(x: f64) -> f64 {
                    $unary_f64(x)
                }
            )*
            $(
                #[inline]
                fn $binary(x: f64, y: f64) -> f64 {
                    $binary_f64(x, y)
                }
            )*
            #[inline]
            fn sincos(x: f64) -> (f64, f64) {
                let (mut sin_value, mut cos_value) = (0.0, 0.0);
                // SAFETY: Both pointers are to locals that outlive the call.
                unsafe { ladybird_math_sincos(x, &raw mut sin_value, &raw mut cos_value) };
                (sin_value, cos_value)
            }
        }

        impl Float for f32 {
            $(
                #[inline]
                fn $unary(x: f32) -> f32 {
                    $unary_f32(x)
                }
            )*
            $(
                #[inline]
                fn $binary(x: f32, y: f32) -> f32 {
                    $binary_f32(x, y)
                }
            )*
            #[inline]
            fn sincos(x: f32) -> (f32, f32) {
                let (mut sin_value, mut cos_value) = (0.0, 0.0);
                // SAFETY: Both pointers are to locals that outlive the call.
                unsafe { ladybird_math_sincosf(x, &raw mut sin_value, &raw mut cos_value) };
                (sin_value, cos_value)
            }
        }

        $(
            #[inline]
            pub fn $unary<T: Float>(x: T) -> T {
                T::$unary(x)
            }
        )*
        $(
            #[inline]
            pub fn $binary<T: Float>(x: T, y: T) -> T {
                T::$binary(x, y)
            }
        )*
    };
}

define_math_functions! {
    unary: [
        acos => ladybird_math_acos, ladybird_math_acosf;
        asin => ladybird_math_asin, ladybird_math_asinf;
        atan => ladybird_math_atan, ladybird_math_atanf;
        cbrt => ladybird_math_cbrt, ladybird_math_cbrtf;
        cos => ladybird_math_cos, ladybird_math_cosf;
        cosh => ladybird_math_cosh, ladybird_math_coshf;
        exp => ladybird_math_exp, ladybird_math_expf;
        exp2 => ladybird_math_exp2, ladybird_math_exp2f;
        expm1 => ladybird_math_expm1, ladybird_math_expm1f;
        log => ladybird_math_log, ladybird_math_logf;
        log10 => ladybird_math_log10, ladybird_math_log10f;
        log1p => ladybird_math_log1p, ladybird_math_log1pf;
        log2 => ladybird_math_log2, ladybird_math_log2f;
        sin => ladybird_math_sin, ladybird_math_sinf;
        sinh => ladybird_math_sinh, ladybird_math_sinhf;
        tan => ladybird_math_tan, ladybird_math_tanf;
        tanh => ladybird_math_tanh, ladybird_math_tanhf;
    ]
    binary: [
        atan2 => ladybird_math_atan2, ladybird_math_atan2f;
        hypot => ladybird_math_hypot, ladybird_math_hypotf;
        pow => ladybird_math_pow, ladybird_math_powf;
    ]
}

#[inline]
pub fn sincos<T: Float>(x: T) -> (T, T) {
    T::sincos(x)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hypot_does_not_overflow_for_large_finite_arguments() {
        assert_eq!(
            hypot(
                f64::from_bits(0x765f_ffff_ffff_fffe),
                f64::from_bits(0x74bf_ffff_ffff_ffff)
            ),
            1.5744403932561433e262
        );
    }

    #[test]
    fn sincos_matches_sin_and_cos() {
        for x in [0.5f64, 1.0, 3.0, 1.0e10] {
            assert_eq!(sincos(x), (sin(x), cos(x)));
        }
        assert_eq!(sincos(0.5f32), (sin(0.5f32), cos(0.5f32)));
    }

    #[test]
    fn inverse_hyperbolic_functions() {
        assert_eq!(acosh(1.0), 0.0);
        assert!(acosh(0.5).is_nan());
        assert_eq!(asinh(-0.0).to_bits(), (-0.0f64).to_bits());
        assert_eq!(atanh(1.0), f64::INFINITY);
        assert!((asinh(1.0) - 0.881_373_587_019_543).abs() < 1e-15);
        assert!((acosh(2.0) - 1.316_957_896_924_816_6).abs() < 1e-15);
        assert!((atanh(0.5) - 0.549_306_144_334_054_9).abs() < 1e-15);
    }
}
