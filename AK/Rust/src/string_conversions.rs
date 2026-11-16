/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

pub trait UnsignedInteger: Copy {
    const ZERO: Self;

    fn checked_mul_add(self, radix: u32, digit: u32) -> Option<Self>;
}

macro_rules! impl_unsigned_integer {
    ($($type:ty),*) => {
        $(
            impl UnsignedInteger for $type {
                const ZERO: Self = 0;

                fn checked_mul_add(self, radix: u32, digit: u32) -> Option<Self> {
                    self.checked_mul(<$type>::try_from(radix).ok()?)?
                        .checked_add(<$type>::try_from(digit).ok()?)
                }
            }
        )*
    };
}

impl_unsigned_integer!(u8, u16, u32, u64, usize);

/// Parses `input` as an unsigned integer made only of radix-`radix` digits, with no sign, whitespace or prefix.
/// Returns `None` if `input` is empty, holds anything else, or does not fit in `T`.
pub fn parse_ascii_digits<T: UnsignedInteger>(input: &[u8], radix: u32) -> Option<T> {
    if input.is_empty() {
        return None;
    }

    input.iter().try_fold(T::ZERO, |value, &byte| {
        value.checked_mul_add(radix, char::from(byte).to_digit(radix)?)
    })
}
