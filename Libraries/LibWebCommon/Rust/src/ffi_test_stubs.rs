/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The AK functions that unit tests reference without linking AK. The tests only use short ASCII strings, which live
//! in the word itself.

#[unsafe(no_mangle)]
extern "C" fn ladybird_utf16_string_unref(_raw: usize) {}

#[unsafe(no_mangle)]
extern "C" fn ladybird_utf16_string_create_uninitialized(_length: usize, _has_latin1_storage: bool) -> usize {
    unreachable!("no unit test makes a string the host owns");
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_utf16_string_from_utf8(_data: *const u8, _length: usize) -> usize {
    unreachable!("no unit test makes a string the host owns");
}
