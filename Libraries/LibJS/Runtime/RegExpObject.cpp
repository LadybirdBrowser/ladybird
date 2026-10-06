/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Runtime/RegExpObject.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

using namespace EmbeddingABI;

ThrowCompletionOr<GC::Ref<RegExpObject>> regexp_create(VM& vm, Value pattern, Value flags)
{
    return completion_from_abi<GC::Ref<RegExpObject>>(js_regexp_create(vm_to_abi(vm), value_to_abi(pattern), value_to_abi(flags)));
}

Utf16String RegExpObject::pattern() const
{
    return owned_utf16_string_from_abi(js_regexp_pattern(object_to_abi(*this)));
}

Utf16String RegExpObject::flags() const
{
    return owned_utf16_string_from_abi(js_regexp_flags(object_to_abi(*this)));
}

}
