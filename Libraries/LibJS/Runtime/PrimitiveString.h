/*
 * Copyright (c) 2020-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <LibGC/CellAllocator.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Utf16String.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

class PrimitiveString final : public Cell {
    GC_CELL(PrimitiveString, Cell);
    GC_DECLARE_ALLOCATOR(PrimitiveString);

public:
    [[nodiscard]] static GC::Ref<PrimitiveString> create(VM&, Utf16String);
    [[nodiscard]] static GC::Ref<PrimitiveString> create(VM&, String);
    [[nodiscard]] static GC::Ref<PrimitiveString> create(VM&, FlyString const&);
    [[nodiscard]] static GC::Ref<PrimitiveString> create(VM&, PrimitiveString&, PrimitiveString&);
    [[nodiscard]] static GC::Ref<PrimitiveString> create(VM&, StringView);

    virtual ~PrimitiveString();

    PrimitiveString(PrimitiveString const&) = delete;
    PrimitiveString& operator=(PrimitiveString const&) = delete;

    bool is_empty() const;

    [[nodiscard]] String utf8_string() const;
    [[nodiscard]] StringView utf8_string_view() const;
    bool has_utf8_string() const { return m_utf8_string.has_value(); }

    [[nodiscard]] Utf16String utf16_string() const;
    [[nodiscard]] Utf16View utf16_string_view() const;
    bool has_utf16_string() const { return m_utf16_string.has_value(); }

    ThrowCompletionOr<Optional<Value>> get(VM&, PropertyKey const&) const;

private:
    explicit PrimitiveString(PrimitiveString&, PrimitiveString&);
    explicit PrimitiveString(String);
    explicit PrimitiveString(Utf16String);

    virtual void visit_edges(Cell::Visitor&) override;

    enum class EncodingPreference {
        UTF8,
        UTF16,
    };
    void resolve_rope_if_needed(EncodingPreference) const;

    mutable bool m_is_rope { false };

    mutable GC::Ptr<PrimitiveString> m_lhs;
    mutable GC::Ptr<PrimitiveString> m_rhs;

    mutable Optional<String> m_utf8_string;
    mutable Optional<Utf16String> m_utf16_string;
};

}
