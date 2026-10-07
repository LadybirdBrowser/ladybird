/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! BigInt values. A magnitude crosses as 32-bit words, least significant first, which is the word order of
//! Crypto::UnsignedBigInteger's words() and of its constructor from them. Every function runs on the thread that owns
//! the VM.

use num_bigint::Sign;

use crate::embedding::abi_types::{
    JSBigInt, JSOwnedUtf16String, cell_from_abi, cell_into_abi, owned_utf16_string_into_abi, vm_from_abi,
};
use crate::layout::host_class::JSVM;
use crate::runtime::big_int::{BigInt, SignedBigInteger};
use crate::runtime::big_int_algorithms;

/// A BigInt of the magnitude in the `word_count` words at `words`, negated if `is_negative` is set. A zero magnitude
/// makes 0n whatever the sign. Call on the VM's thread.
///
/// # Safety
///
/// `vm` must be the embedder's VM, and `words` must point to `word_count` words unless `word_count` is 0.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_bigint_create_from_magnitude(
    vm: *mut JSVM,
    is_negative: bool,
    words: *const u32,
    word_count: usize,
) -> *mut JSBigInt {
    // SAFETY: The caller passes its VM.
    let vm = unsafe { vm_from_abi(vm) };
    let words: &[u32] = if word_count == 0 {
        &[]
    } else {
        // SAFETY: The caller passes `word_count` words.
        unsafe { core::slice::from_raw_parts(words, word_count) }
    };
    let sign = if is_negative { Sign::Minus } else { Sign::Plus };
    cell_into_abi(BigInt::create(vm, SignedBigInteger::from_slice(sign, words)))
}

/// Whether `bigint` is less than 0n. Call on the VM's thread.
///
/// # Safety
///
/// `bigint` must be a BigInt of the embedder's VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_bigint_is_negative(bigint: *mut JSBigInt) -> bool {
    // SAFETY: The caller passes a BigInt of the VM.
    unsafe { cell_from_abi(bigint) }.big_integer().sign() == Sign::Minus
}

/// How many words the magnitude of `bigint` has, without leading zero words, so 0 for 0n. Call on the VM's thread.
///
/// # Safety
///
/// `bigint` must be a BigInt of the embedder's VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_bigint_magnitude_word_count(bigint: *mut JSBigInt) -> usize {
    // SAFETY: The caller passes a BigInt of the VM.
    unsafe { cell_from_abi(bigint) }.big_integer().iter_u32_digits().len()
}

/// Writes the magnitude of `bigint` to the `word_count` words at `words`, which must be the count
/// js_bigint_magnitude_word_count returns. Call on the VM's thread.
///
/// # Safety
///
/// `bigint` must be a BigInt of the embedder's VM, and `words` valid for writing `word_count` words unless
/// `word_count` is 0.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_bigint_copy_magnitude_words(bigint: *mut JSBigInt, words: *mut u32, word_count: usize) {
    // SAFETY: The caller passes a BigInt of the VM.
    let bigint = unsafe { cell_from_abi(bigint) };
    let magnitude = bigint.big_integer().iter_u32_digits();
    assert_eq!(
        magnitude.len(),
        word_count,
        "the embedder makes room for the whole magnitude"
    );
    if word_count == 0 {
        return;
    }
    // SAFETY: The caller passes room for `word_count` words.
    let words = unsafe { core::slice::from_raw_parts_mut(words, word_count) };
    for (word, magnitude_word) in words.iter_mut().zip(magnitude) {
        *word = magnitude_word;
    }
}

/// The digits of `bigint` in `radix`, which is 2 to 36, with lowercase letters and a leading "-" if it is negative,
/// as BigInt.prototype.toString writes them. The caller owns the string. Call on the VM's thread.
///
/// # Safety
///
/// `bigint` must be a BigInt of the embedder's VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_bigint_to_string(bigint: *mut JSBigInt, radix: u32) -> JSOwnedUtf16String {
    assert!((2..=36).contains(&radix), "the embedder passes a radix from 2 to 36");
    // SAFETY: The caller passes a BigInt of the VM.
    let digits = big_int_algorithms::to_base(unsafe { cell_from_abi(bigint) }.big_integer(), radix);
    owned_utf16_string_into_abi(ak::Utf16String::from_utf8(&digits))
}
