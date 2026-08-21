/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSS math-function names generated from the same source as the C++ parser.

include!(concat!(env!("OUT_DIR"), "/math_functions_generated.rs"));

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn recognizes_generated_math_function_names_case_insensitively() {
        assert_eq!(
            math_function_from_name(&"CaLc".encode_utf16().collect::<Vec<_>>()),
            Some(MathFunction::Calc)
        );
        assert_eq!(
            math_function_from_name(&"HyPoT".encode_utf16().collect::<Vec<_>>()),
            Some(MathFunction::Hypot)
        );
        assert_eq!(
            math_function_from_name(&"unknown".encode_utf16().collect::<Vec<_>>()),
            None
        );
    }
}
