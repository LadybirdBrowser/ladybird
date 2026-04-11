/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/String.h>
#include <AK/Vector.h>

namespace WebView {

inline ByteString history_log_suggestions(Vector<String> const& suggestions)
{
    return ByteString::formatted("[{}]", ByteString::join(", "sv, suggestions));
}

}
