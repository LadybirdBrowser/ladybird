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

    [[nodiscard]] Utf16String utf16_string() const;
    // The code units stay valid as long as the string does.
    [[nodiscard]] Utf16View utf16_string_view() const;

    size_t length_in_utf16_code_units() const;

    [[nodiscard]] bool operator==(PrimitiveString const&) const;
};

}
