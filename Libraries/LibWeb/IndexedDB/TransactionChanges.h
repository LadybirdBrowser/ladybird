/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/JsonValue.h>
#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>

namespace Web::IndexedDB {

struct TransactionChange {
    Utf16String database_name;
    Utf16String object_store_name;
    Optional<JsonValue> key {};
};

struct TransactionChanges {
    Vector<TransactionChange> added;
    Vector<TransactionChange> changed;
    Vector<TransactionChange> deleted;

    bool is_empty() const
    {
        return added.is_empty() && changed.is_empty() && deleted.is_empty();
    }
};

}
