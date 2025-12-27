/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Types.h>

namespace WebView {

enum class StorageOperationError : u8 {
    QuotaExceededError,
};

// Error setting the storage item, or the old value if the operation was successful.
using StorageSetResult = Variant<StorageOperationError, Optional<String>>;

}
