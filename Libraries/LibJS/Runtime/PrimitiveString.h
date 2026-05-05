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
#include <AK/Utf16String.h>
#include <LibGC/CellAllocator.h>
#include <LibJS/Export.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

class JS_API PrimitiveString : public Cell {
    GC_CELL(PrimitiveString, Cell);
    GC_DECLARE_ALLOCATOR(PrimitiveString);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    [[nodiscard]] static GC::Ref<PrimitiveString> create(VM&, Utf16String const&);
    [[nodiscard]] static GC::Ref<PrimitiveString> create(VM&, Utf16View const&);
    [[nodiscard]] static GC::Ref<PrimitiveString> create(VM&, Utf16FlyString const&);

    [[nodiscard]] static GC::Ref<PrimitiveString> create(VM&, String const&);
    [[nodiscard]] static GC::Ref<PrimitiveString> create(VM&, StringView);
    [[nodiscard]] static GC::Ref<PrimitiveString> create(VM&, FlyString const&);

    [[nodiscard]] static GC::Ref<PrimitiveString> create(VM&, PrimitiveString&, PrimitiveString&);
    [[nodiscard]] static GC::Ref<PrimitiveString> create(VM&, PrimitiveString const&, size_t code_unit_offset, size_t code_unit_length);

    [[nodiscard]] static GC::Ref<PrimitiveString> create_from_unsigned_integer(VM&, u64);

    virtual ~PrimitiveString() override;

    PrimitiveString(PrimitiveString const&) = delete;
    PrimitiveString& operator=(PrimitiveString const&) = delete;

    bool is_empty() const;

    [[nodiscard]] String utf8_string() const;
    [[nodiscard]] StringView utf8_string_view() const;
    bool has_utf8_string() const { return m_utf8_string.has_value(); }

    [[nodiscard]] Utf16String utf16_string() const;
    [[nodiscard]] Utf16View utf16_string_view() const;
    bool has_utf16_string() const { return m_utf16_string.has_value(); }

    size_t length_in_utf16_code_units() const;

    ThrowCompletionOr<Optional<Value>> get(VM&, PropertyKey const&) const;

    [[nodiscard]] bool operator==(PrimitiveString const&) const;

protected:
    enum class DeferredKind : u8 {
        None,
        Rope,
        Substring,
    };

    explicit PrimitiveString(DeferredKind deferred_kind)
        : m_deferred_kind(deferred_kind)
    {
    }

    mutable DeferredKind m_deferred_kind { DeferredKind::None };

    mutable Optional<String> m_utf8_string;
    mutable Optional<Utf16String> m_utf16_string;

    bool m_utf8_string_is_in_cache { false };
    bool m_utf16_string_is_in_cache { false };

    enum class EncodingPreference {
        UTF8,
        UTF16,
    };

private:
    friend class RopeString;
    friend class Substring;

    virtual void finalize() override;
    virtual size_t external_memory_size() const override;

    explicit PrimitiveString(Utf16String);
    explicit PrimitiveString(String);

    void resolve_if_needed(EncodingPreference) const;
    Optional<StringView> short_flat_string_storage_view() const;
    static GC::Ptr<PrimitiveString> try_create_short_flat_concatenated_string(VM&, PrimitiveString const& lhs, PrimitiveString const& rhs);
};

class RopeString final : public PrimitiveString {
    GC_CELL(RopeString, PrimitiveString);
    GC_DECLARE_ALLOCATOR(RopeString);

public:
    virtual ~RopeString() override;

private:
    friend class PrimitiveString;

    explicit RopeString(GC::Ref<PrimitiveString>, GC::Ref<PrimitiveString>);

    virtual void visit_edges(Visitor&) override;

    void resolve(EncodingPreference) const;

    mutable GC::Ptr<PrimitiveString> m_lhs;
    mutable GC::Ptr<PrimitiveString> m_rhs;
};

class Substring final : public PrimitiveString {
    GC_CELL(Substring, PrimitiveString);
    GC_DECLARE_ALLOCATOR(Substring);

public:
    virtual ~Substring() override;

private:
    friend class PrimitiveString;

    explicit Substring(GC::Ref<PrimitiveString>, size_t code_unit_offset, size_t code_unit_length);

    virtual void visit_edges(Visitor&) override;

    void resolve(EncodingPreference) const;

    mutable GC::Ptr<PrimitiveString> m_source_string;
    size_t m_code_unit_offset { 0 };
    size_t m_code_unit_length { 0 };
};

}
