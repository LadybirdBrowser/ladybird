/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::Utf16String;
use libjs_runtime_macros::Trace;
use num_traits::FromPrimitive;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::value::Value;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;

/// The arbitrary-precision integer a BigInt holds, in place of Crypto::SignedBigInteger.
pub use num_bigint::BigInt as SignedBigInteger;

#[repr(C)]
#[derive(Trace)]
pub struct BigInt {
    header: CellHeader,
    big_integer: SignedBigInteger,
}

define_cell!(BigInt, BigInt);

impl BigInt {
    fn new(big_integer: SignedBigInteger) -> Self {
        Self {
            header: CellHeader::for_class(Self::CLASS),
            big_integer,
        }
    }

    pub fn create(vm: &Vm, big_integer: SignedBigInteger) -> Gc<BigInt> {
        vm.heap().allocate(Self::new(big_integer))
    }

    pub fn big_integer(&self) -> &SignedBigInteger {
        &self.big_integer
    }

    pub fn to_utf16_string(&self) -> Utf16String {
        Utf16String::from_utf8(&format!("{}n", self.big_integer))
    }
}

// 21.2.1.1.1 NumberToBigInt ( number ), https://tc39.es/ecma262/#sec-numbertobigint
pub fn number_to_bigint(vm: &Vm, number: Value) -> ThrowCompletionOr<Gc<BigInt>> {
    assert!(number.is_number());

    // 1. If IsIntegralNumber(number) is false, throw a RangeError exception.
    if !number.is_integral_number() {
        return vm.throw_completion(ErrorKind::RangeError, ErrorType::BigIntFromNonIntegral, &[]);
    }

    // 2. Return the BigInt value that represents ℝ(number).
    let big_integer = SignedBigInteger::from_f64(number.as_f64()).expect("an integral number is finite");
    Ok(BigInt::create(vm, big_integer))
}
