/*
 * Copyright (c) 2020-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <LibGC/Ptr.h>
#include <LibJS/Embedding/Layout.h>
#include <LibJS/Export.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/EngineCell.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

class JS_API PrimitiveString : public EngineCell {
public:
    [[nodiscard]] static GC::Ref<PrimitiveString> create(VM&, Utf16String const&);
    [[nodiscard]] static GC::Ref<PrimitiveString> create(VM&, Utf16View const&);
    [[nodiscard]] static GC::Ref<PrimitiveString> create(VM&, Utf16FlyString const&);

    [[nodiscard]] static GC::Ref<PrimitiveString> create_from_unsigned_integer(VM&, u64);

    // The string that holds the code units, which stays where it is, unchanged, as long as this string lives.
    [[nodiscard]] ALWAYS_INLINE Utf16String const& utf16_string() const
    {
        // OPTIMIZATION: A string that is not deferred holds the string of its code units, which is read where it is.
        auto deferred_kind = *(reinterpret_cast<u8 const*>(this) + JS_LAYOUT_PRIMITIVE_STRING_DEFERRED_KIND_AND_FLAGS_OFFSET) & JS_LAYOUT_PRIMITIVE_STRING_DEFERRED_KIND_MASK;
        if (deferred_kind == JS_LAYOUT_PRIMITIVE_STRING_DEFERRED_KIND_NONE) [[likely]]
            return *reinterpret_cast<Utf16String const*>(reinterpret_cast<u8 const*>(this) + JS_LAYOUT_PRIMITIVE_STRING_UTF16_STRING_OFFSET);
        return resolved_utf16_string();
    }
    // The code units stay valid as long as the string does.
    [[nodiscard]] Utf16View utf16_string_view() const;

    size_t length_in_utf16_code_units() const;

    [[nodiscard]] bool operator==(PrimitiveString const&) const;

private:
    static_assert(JS_LAYOUT_PRIMITIVE_STRING_DEFERRED_KIND_AND_FLAGS_SIZE == sizeof(u8));
    static_assert(JS_LAYOUT_PRIMITIVE_STRING_UTF16_STRING_SIZE == sizeof(Utf16String));
    static_assert(JS_LAYOUT_PRIMITIVE_STRING_UTF16_STRING_OFFSET % alignof(Utf16String) == 0);

    // The string that holds the code units, after resolving a string the VM built lazily.
    [[nodiscard]] Utf16String const& resolved_utf16_string() const;
};

}
