/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Runtime/Symbol.h>

namespace JS {

using namespace EmbeddingABI;

Utf16String Symbol::descriptive_string() const
{
    return owned_utf16_string_from_abi(js_symbol_descriptive_string(symbol_to_abi(*this)));
}

}
