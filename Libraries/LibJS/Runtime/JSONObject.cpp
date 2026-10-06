/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Runtime/JSONObject.h>

namespace JS {

using namespace EmbeddingABI;

// 25.5.2 JSON.stringify ( value [ , replacer [ , space ] ] ), https://tc39.es/ecma262/#sec-json.stringify
ThrowCompletionOr<Optional<Utf16String>> JSONObject::stringify_impl(VM& vm, Value value, Value replacer, Value space)
{
    JSOwnedUtf16String string {};
    if (!TRY(completion_from_abi<bool>(js_json_stringify(vm_to_abi(vm), value_to_abi(value), value_to_abi(replacer), value_to_abi(space), &string))))
        return OptionalNone {};
    return owned_utf16_string_from_abi(string);
}

// 25.5.1.1 ParseJSON ( text ), https://tc39.es/ecma262/#sec-ParseJSON
ThrowCompletionOr<Value> JSONObject::parse_json(VM& vm, Utf16View text, JSONParseRecord* root_record)
{
    VERIFY(!root_record);
    return completion_from_abi<Value>(js_json_parse(vm_to_abi(vm), utf16_view_to_abi(text)));
}

}
