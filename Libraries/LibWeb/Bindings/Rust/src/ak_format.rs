/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Numbers in the messages of exceptions, written as LibWeb's C++ wrote them.

use core::fmt::LowerExp;

/// The number as AK's formatter writes it for "{}", which the messages of the C++ had: the shortest digits that
/// read back as the number, in the notation of Number::toString() of ECMAScript.
pub(crate) fn ak_formatted(number: impl LowerExp) -> String {
    let scientific = format!("{number:e}");
    let (sign, scientific) = match scientific.strip_prefix('-') {
        Some(magnitude) => ("-", magnitude),
        None => ("", scientific.as_str()),
    };
    let (mantissa, exponent) = scientific
        .split_once('e')
        .expect("a number in scientific notation has an exponent");
    let exponent: i32 = exponent.parse().expect("the exponent is an integer");
    let digits: String = mantissa.chars().filter(|character| *character != '.').collect();
    let digit_count = i32::try_from(digits.len()).expect("a number has few digits");
    let zeros = |count: i32| "0".repeat(usize::try_from(count).expect("the count is not negative"));

    let unsigned = if !(-6..=20).contains(&exponent) {
        let (first, rest) = digits.split_at(1);
        let fraction = if rest.is_empty() {
            String::new()
        } else {
            format!(".{rest}")
        };
        let exponent_sign = if exponent < 0 { '-' } else { '+' };
        format!("{first}{fraction}e{exponent_sign}{}", exponent.unsigned_abs())
    } else if exponent < 0 {
        format!("0.{}{digits}", zeros(-exponent - 1))
    } else if digit_count <= exponent + 1 {
        format!("{digits}{}", zeros(exponent + 1 - digit_count))
    } else {
        let (integer, fraction) = digits.split_at(usize::try_from(exponent + 1).expect("the exponent is not negative"));
        format!("{integer}.{fraction}")
    };
    format!("{sign}{unsigned}")
}

#[cfg(test)]
mod tests {
    use super::ak_formatted;

    #[test]
    fn numbers_are_formatted_as_ak_formats_them() {
        // NB: What AK's "{}" wrote for these numbers, which a C++ program printed.
        let floats: [(f32, &str); 14] = [
            (-1.0, "-1"),
            (-0.5, "-0.5"),
            (-0.1, "-0.1"),
            (-1e30, "-1e+30"),
            (-1e-7, "-1e-7"),
            (-123_456_789.0, "-123456790"),
            (-3.402_823_5e38, "-3.4028235e+38"),
            (-1.175_494_4e-38, "-1.1754944e-38"),
            (-0.333_333_3, "-0.3333333"),
            (-16_777_217.0, "-16777216"),
            (-1e7, "-10000000"),
            (-1e16, "-10000000000000000"),
            (-100_000.0, "-100000"),
            (-0.0001, "-0.0001"),
        ];
        for (number, formatted) in floats {
            assert_eq!(ak_formatted(number), formatted);
        }
        let doubles: [(f64, &str); 10] = [
            (-1.0, "-1"),
            (-0.1, "-0.1"),
            (-1e300, "-1e+300"),
            (-1e-7, "-1e-7"),
            (-123_456_789.123, "-123456789.123"),
            (-1e21, "-1e+21"),
            (-1e22, "-1e+22"),
            (-0.0001, "-0.0001"),
            (-1e16, "-10000000000000000"),
            (-1e17, "-100000000000000000"),
        ];
        for (number, formatted) in doubles {
            assert_eq!(ak_formatted(number), formatted);
        }
    }
}
