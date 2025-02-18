/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/String.h>

namespace URL::Pattern {

// NOTE: All exceptions which are thrown by the URLPattern spec are TypeErrors which web-based callers are expected to assume.
//       If this ever does not become the case, this should change to also include the error type.
struct ErrorInfo {
    String message;
};

template<typename ValueT>
using PatternErrorOr = AK::ErrorOr<ValueT, ErrorInfo>;

}
