/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace WebView {

enum class StorageOperationError : u8 {
    None,
    QuotaExceededError,
};

}
