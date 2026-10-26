/*
 * Copyright (c) 2025, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Key generators, https://w3c.github.io/IndexedDB/#key-generator-construct

// https://w3c.github.io/IndexedDB/#key-generator-construct

// The current number is always a positive integer less than or equal to 2^53 (9007199254740992) + 1.
pub(crate) const MAX_KEY_GENERATOR_VALUE: f64 = 9007199254740992.0;

pub(crate) struct KeyGenerator {
    // A key generator has a current number.
    // The current number is always a positive integer less than or equal to 2^53 (9007199254740992) + 1.
    // The initial value of a key generator's current number is 1, set when the associated object store is created.
    // The current number is incremented as keys are generated, and may be updated to a specific value by using explicit keys.
    current_number: u64,
}

impl KeyGenerator {
    pub(crate) fn new() -> Self {
        Self { current_number: 1 }
    }

    pub(crate) fn current_number(&self) -> u64 {
        self.current_number
    }

    pub(crate) fn increment(&mut self, amount: u64) {
        self.current_number += amount;
    }

    pub(crate) fn set(&mut self, value: u64) {
        self.current_number = value;
    }
}
