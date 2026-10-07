/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::layout::value::Value;

// 24.5.1 CanonicalizeKeyedCollectionKey ( key ), https://tc39.es/ecma262/#sec-canonicalizekeyedcollectionkey
pub fn canonicalize_keyed_collection_key(key: Value) -> Value {
    // 1. If key is -0𝔽, return +0𝔽.
    if key.is_negative_zero() {
        return Value::from_f64(0.0);
    }

    // 2. Return key.
    key
}
