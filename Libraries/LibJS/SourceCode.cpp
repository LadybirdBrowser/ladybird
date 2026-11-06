/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/ScriptAndModuleABIConversions.h>
#include <LibJS/SourceCode.h>
#include <LibTextCodec/Decoder.h>

namespace JS {

using namespace EmbeddingABI;

static_assert(sizeof(Utf16String) == sizeof(JSOwnedUtf16String));
static_assert(alignof(Utf16String) == alignof(JSOwnedUtf16String));

// The source code's string, which the runtime keeps in an AK::Utf16String of its own for as long as the source code
// lives.
static Utf16String const& string_in_place_from_abi(JSOwnedUtf16String const* string)
{
    return *reinterpret_cast<Utf16String const*>(string);
}

NonnullRefPtr<SourceCode const> SourceCode::create(Utf16String filename, Utf16String code)
{
    return adopt_source_code_from_abi(js_source_code_create(owned_utf16_string_to_abi(move(filename)), owned_utf16_string_to_abi(move(code))));
}

Utf16String const& SourceCode::filename() const
{
    return string_in_place_from_abi(js_source_code_filename_address(source_code_to_abi(*this)));
}

Utf16String const& SourceCode::code() const
{
    return string_in_place_from_abi(js_source_code_code_address(source_code_to_abi(*this)));
}

size_t SourceCode::length_in_code_units() const
{
    return js_source_code_length_in_code_units(source_code_to_abi(*this));
}

Utf16String SourceCode::source_text_from_offsets(size_t start_offset, size_t length) const
{
    if (length == 0)
        return {};

    VERIFY(start_offset <= NumericLimits<size_t>::max() - length);
    return Utf16String::from_utf16(code().utf16_view().substring_view(start_offset, length));
}

void SourceCode::ref() const
{
    js_source_code_retain(source_code_to_abi(*this));
}

void SourceCode::unref() const
{
    js_source_code_release(source_code_to_abi(*this));
}

}
