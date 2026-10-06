/*
 * Copyright (c) 2020-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/ObjectEmbeddingABIConversions.h>
#include <LibJS/Runtime/FunctionObject.h>

namespace JS {

using namespace EmbeddingABI;

Realm* FunctionObject::realm() const
{
    return cell_from_abi<Realm>(js_function_realm(object_to_abi(*this)));
}

Utf16String FunctionObject::name_for_call_stack() const
{
    return owned_utf16_string_from_abi(js_function_name_for_call_stack(object_to_abi(*this)));
}

}
