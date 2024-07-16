/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/KeyedCollections.h>

namespace JS {

// 24.5.1 CanonicalizeKeyedCollectionKey ( key ), https://tc39.es/ecma262/#sec-canonicalizekeyedcollectionkey
Value canonicalize_keyed_collection_key(Value key)
{
    // 1. If key is -0𝔽, return +0𝔽.
    if (key.is_negative_zero())
        return Value { 0.0 };

    // 2. Return key.
    return key;
}

}
