/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::Utf16String;

use crate::layout::value::Value;
use crate::unicode::number_format::NumberFormatValue;

/// big_integer().to_base_utf16(10) of a BigInt value.
pub fn bigint_to_base_10(value: Value) -> Utf16String {
    Utf16String::from_utf8(&value.as_bigint().big_integer().to_string())
}

/// The symbols an Intl mathematical value may be besides a mathematical value.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MathematicalValueSymbol {
    PositiveInfinity,
    NegativeInfinity,
    NegativeZero,
    NotANumber,
}

/// https://tc39.es/ecma402/#intl-mathematical-value
#[derive(Clone)]
pub enum MathematicalValue {
    Number(f64),
    String(Utf16String),
    Symbol(MathematicalValueSymbol),
}

impl Default for MathematicalValue {
    fn default() -> Self {
        MathematicalValue::Number(0.0)
    }
}

impl MathematicalValue {
    pub fn from_number(number: f64) -> Self {
        Self::value_from_number(number)
    }

    pub fn from_string(string: Utf16String) -> Self {
        MathematicalValue::String(string)
    }

    pub fn from_symbol(symbol: MathematicalValueSymbol) -> Self {
        MathematicalValue::Symbol(symbol)
    }

    /// MathematicalValue(Value), of a Number or a BigInt.
    pub fn from_value(value: Value) -> Self {
        if value.is_number() {
            return Self::value_from_number(value.as_f64());
        }

        MathematicalValue::String(bigint_to_base_10(value))
    }

    pub fn is_number(&self) -> bool {
        matches!(self, MathematicalValue::Number(_))
    }

    pub fn as_number(&self) -> f64 {
        match self {
            MathematicalValue::Number(number) => *number,
            _ => unreachable!("the mathematical value is a number"),
        }
    }

    pub fn is_string(&self) -> bool {
        matches!(self, MathematicalValue::String(_))
    }

    pub fn as_string(&self) -> &Utf16String {
        match self {
            MathematicalValue::String(string) => string,
            _ => unreachable!("the mathematical value is a string"),
        }
    }

    pub fn is_mathematical_value(&self) -> bool {
        self.is_number() || self.is_string()
    }

    pub fn is_positive_infinity(&self) -> bool {
        matches!(
            self,
            MathematicalValue::Symbol(MathematicalValueSymbol::PositiveInfinity)
        )
    }

    pub fn is_negative_infinity(&self) -> bool {
        matches!(
            self,
            MathematicalValue::Symbol(MathematicalValueSymbol::NegativeInfinity)
        )
    }

    pub fn is_negative_zero(&self) -> bool {
        matches!(self, MathematicalValue::Symbol(MathematicalValueSymbol::NegativeZero))
    }

    pub fn is_nan(&self) -> bool {
        matches!(self, MathematicalValue::Symbol(MathematicalValueSymbol::NotANumber))
    }

    pub fn to_value(&self) -> NumberFormatValue {
        match self {
            MathematicalValue::Number(number) => NumberFormatValue::Number(*number),
            MathematicalValue::String(string) => NumberFormatValue::String(string.clone()),
            MathematicalValue::Symbol(symbol) => NumberFormatValue::Number(match symbol {
                MathematicalValueSymbol::PositiveInfinity => f64::INFINITY,
                MathematicalValueSymbol::NegativeInfinity => f64::NEG_INFINITY,
                MathematicalValueSymbol::NegativeZero => -0.0,
                MathematicalValueSymbol::NotANumber => f64::NAN,
            }),
        }
    }

    fn value_from_number(number: f64) -> Self {
        if number == f64::INFINITY {
            return MathematicalValue::Symbol(MathematicalValueSymbol::PositiveInfinity);
        }
        if number == f64::NEG_INFINITY {
            return MathematicalValue::Symbol(MathematicalValueSymbol::NegativeInfinity);
        }
        if number == 0.0 && number.is_sign_negative() {
            return MathematicalValue::Symbol(MathematicalValueSymbol::NegativeZero);
        }
        if number.is_nan() {
            return MathematicalValue::Symbol(MathematicalValueSymbol::NotANumber);
        }
        MathematicalValue::Number(number)
    }
}
