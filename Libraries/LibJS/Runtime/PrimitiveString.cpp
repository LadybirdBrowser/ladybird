/*
 * Copyright (c) 2020-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/FlyString.h>
#include <AK/StringBuilder.h>
#include <AK/Utf16FlyString.h>
#include <AK/Utf16StringBuilder.h>
#include <AK/Utf16View.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/ExternalMemory.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/PropertyKey.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

// Strings shorter than or equal to this length are cached in the VM and deduplicated.
// Longer strings are not cached to avoid excessive hashing and lookup costs.
static constexpr size_t MAX_LENGTH_FOR_STRING_CACHE = 256;

GC_DEFINE_ALLOCATOR(PrimitiveString);
GC_DEFINE_ALLOCATOR(RopeString);
GC_DEFINE_ALLOCATOR(Substring);

Optional<StringView> PrimitiveString::short_flat_string_storage_view() const
{
    if (m_deferred_kind != DeferredKind::None)
        return {};

    if (m_utf16_string.has_value() && m_utf16_string->has_short_ascii_storage())
        return m_utf16_string->ascii_view();

    return {};
}

GC::Ptr<PrimitiveString> PrimitiveString::try_create_short_flat_concatenated_string(VM& vm, PrimitiveString const& lhs, PrimitiveString const& rhs)
{
    auto lhs_view = lhs.short_flat_string_storage_view();
    if (!lhs_view.has_value())
        return nullptr;

    auto rhs_view = rhs.short_flat_string_storage_view();
    if (!rhs_view.has_value())
        return nullptr;

    auto const byte_count = lhs_view->length() + rhs_view->length();
    if (byte_count > String::MAX_SHORT_STRING_BYTE_COUNT)
        return nullptr;

    auto string = Utf16String::create_uninitialized_ascii(byte_count, [&](Bytes buffer) {
        lhs_view->bytes().copy_to(buffer.slice(0, lhs_view->length()));
        rhs_view->bytes().copy_to(buffer.slice(lhs_view->length()));
    });

    return PrimitiveString::create(vm, string);
}

GC::Ref<PrimitiveString> PrimitiveString::create(VM& vm, Utf16String const& string)
{
    if (string.is_empty())
        return vm.empty_string();

    auto const length_in_code_units = string.length_in_code_units();

    if (length_in_code_units == 1) {
        if (auto code_unit = string.code_unit_at(0); is_ascii(code_unit))
            return vm.single_ascii_character_string(static_cast<u8>(code_unit));
    }

    if (length_in_code_units > MAX_LENGTH_FOR_STRING_CACHE) {
        return vm.heap().allocate<PrimitiveString>(string);
    }

    auto& string_cache = vm.utf16_string_cache();
    if (auto it = string_cache.find(string); it != string_cache.end())
        return *it->value;

    auto new_string = vm.heap().allocate<PrimitiveString>(string);
    new_string->m_utf16_string_is_in_cache = true;
    string_cache.set(move(string), new_string);
    return *new_string;
}

GC::Ref<PrimitiveString> PrimitiveString::create(VM& vm, Utf16View const& string)
{
    return create(vm, Utf16String::from_utf16(string));
}

GC::Ref<PrimitiveString> PrimitiveString::create(VM& vm, Utf16FlyString const& string)
{
    return create(vm, string.to_utf16_string());
}

GC::Ref<PrimitiveString> PrimitiveString::create_from_unsigned_integer(VM& vm, u64 number)
{
    if (number < vm.numeric_string_cache().size()) {
        auto& cache_slot = vm.numeric_string_cache()[number];
        if (!cache_slot) {
            auto string = Utf16String::number(number);
            cache_slot = create(vm, string);
        }
        return *cache_slot;
    }
    return create(vm, Utf16String::number(number));
}

GC::Ref<PrimitiveString> PrimitiveString::create(VM& vm, PrimitiveString& lhs, PrimitiveString& rhs)
{
    // We're here to concatenate two strings into a new rope string. However, if any of them are empty, no rope is required.
    bool lhs_empty = lhs.is_empty();
    bool rhs_empty = rhs.is_empty();

    if (lhs_empty && rhs_empty)
        return vm.empty_string();

    if (lhs_empty)
        return rhs;

    if (rhs_empty)
        return lhs;

    if (auto short_flat_string = try_create_short_flat_concatenated_string(vm, lhs, rhs))
        return *short_flat_string;

    return vm.heap().allocate<RopeString>(lhs, rhs);
}

GC::Ref<PrimitiveString> PrimitiveString::create(VM& vm, PrimitiveString const& string, size_t code_unit_offset, size_t code_unit_length)
{
    auto string_length = string.length_in_utf16_code_units();
    VERIFY(code_unit_offset <= string_length);
    VERIFY(code_unit_length <= string_length - code_unit_offset);

    if (code_unit_length == 0)
        return vm.empty_string();

    if (code_unit_offset == 0 && code_unit_length == string_length)
        return const_cast<PrimitiveString&>(string);

    if (code_unit_length == 1) {
        if (auto code_unit = string.utf16_string_view().code_unit_at(code_unit_offset); is_ascii(code_unit))
            return vm.single_ascii_character_string(static_cast<u8>(code_unit));
    }

    if (string.m_deferred_kind == DeferredKind::Substring) {
        auto const& substring = static_cast<Substring const&>(string);
        return create(vm, *substring.m_source_string, substring.m_code_unit_offset + code_unit_offset, code_unit_length);
    }

    return vm.heap().allocate<Substring>(const_cast<PrimitiveString&>(string), code_unit_offset, code_unit_length);
}

PrimitiveString::PrimitiveString(Utf16String string)
    : m_utf16_string(move(string))
{
}

PrimitiveString::~PrimitiveString() = default;

size_t PrimitiveString::external_memory_size() const
{
    size_t size = 0;
    if (m_utf16_string.has_value())
        size = saturating_add_external_memory_size(size, utf16_string_external_memory_size(*m_utf16_string));
    return size;
}

void PrimitiveString::finalize()
{
    Base::finalize();
    if (m_utf16_string_is_in_cache) {
        auto const& string = *m_utf16_string;
        if (string.length_in_code_units() <= MAX_LENGTH_FOR_STRING_CACHE)
            vm().utf16_string_cache().remove(string);
    }
}

bool PrimitiveString::is_empty() const
{
    if (m_deferred_kind == DeferredKind::Rope) {
        // NOTE: We never make an empty rope string.
        return false;
    }
    if (m_deferred_kind == DeferredKind::Substring)
        return static_cast<Substring const&>(*this).m_code_unit_length == 0;

    if (has_utf16_string())
        return m_utf16_string->is_empty();
    VERIFY_NOT_REACHED();
}

Utf16String PrimitiveString::utf16_string() const
{
    resolve_if_needed();

    VERIFY(has_utf16_string());
    return *m_utf16_string;
}

Utf16View PrimitiveString::utf16_string_view() const
{
    if (!has_utf16_string()) {
        if (m_deferred_kind == DeferredKind::Substring) {
            auto const& substring = static_cast<Substring const&>(*this);
            return substring.m_source_string->utf16_string_view().substring_view(substring.m_code_unit_offset, substring.m_code_unit_length);
        }
        (void)utf16_string();
    }
    return *m_utf16_string;
}

size_t PrimitiveString::length_in_utf16_code_units() const
{
    if (m_deferred_kind == DeferredKind::Substring)
        return static_cast<Substring const&>(*this).m_code_unit_length;
    return utf16_string_view().length_in_code_units();
}

bool PrimitiveString::operator==(PrimitiveString const& other) const
{
    if (this == &other)
        return true;
    if (length_in_utf16_code_units() != other.length_in_utf16_code_units())
        return false;
    if (m_utf16_string.has_value() && other.m_utf16_string.has_value())
        return *m_utf16_string == *other.m_utf16_string;
    return utf16_string_view() == other.utf16_string_view();
}

ThrowCompletionOr<Optional<Value>> PrimitiveString::get(VM& vm, PropertyKey const& property_key) const
{
    if (property_key.is_symbol())
        return Optional<Value> {};

    if (property_key.is_string()) {
        if (property_key.as_string() == vm.names.length.as_string()) {
            return Value(static_cast<double>(length_in_utf16_code_units()));
        }
    }

    auto index = canonical_numeric_index_string(property_key, CanonicalIndexMode::IgnoreNumericRoundtrip);
    if (!index.is_index())
        return Optional<Value> {};

    auto string = utf16_string_view();
    if (string.length_in_code_units() <= index.as_index())
        return Optional<Value> {};

    return create(vm, *this, index.as_index(), 1);
}

void PrimitiveString::resolve_if_needed() const
{
    switch (m_deferred_kind) {
    case DeferredKind::None:
        return;
    case DeferredKind::Rope:
        static_cast<RopeString const&>(*this).resolve();
        return;
    case DeferredKind::Substring:
        static_cast<Substring const&>(*this).resolve();
        return;
    }

    VERIFY_NOT_REACHED();
}

void RopeString::resolve() const
{

    // This vector will hold all the pieces of the rope that need to be assembled
    // into the resolved string.
    Vector<PrimitiveString const*, 2> pieces;
    size_t length_in_utf16_code_units = 0;

    // NOTE: We traverse the rope tree without using recursion, since we'd run out of
    //       stack space quickly when handling a long sequence of unresolved concatenations.
    Vector<PrimitiveString const*, 2> stack;
    stack.append(m_rhs);
    stack.append(m_lhs);
    while (!stack.is_empty()) {
        auto const* current = stack.take_last();
        if (current->m_deferred_kind == DeferredKind::Rope) {
            auto& current_rope_string = static_cast<RopeString const&>(*current);
            stack.append(current_rope_string.m_rhs);
            stack.append(current_rope_string.m_lhs);
            continue;
        }

        length_in_utf16_code_units += current->length_in_utf16_code_units();
        pieces.append(current);
    }

    Utf16StringBuilder builder(length_in_utf16_code_units);
    for (auto const* current : pieces) {
        builder.append(current->utf16_string_view());
    }

    m_utf16_string = builder.to_string();
    m_deferred_kind = DeferredKind::None;
    m_lhs = nullptr;
    m_rhs = nullptr;
}

RopeString::RopeString(GC::Ref<PrimitiveString> lhs, GC::Ref<PrimitiveString> rhs)
    : PrimitiveString(DeferredKind::Rope)
    , m_lhs(lhs)
    , m_rhs(rhs)
{
}

RopeString::~RopeString() = default;

void RopeString::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_lhs);
    visitor.visit(m_rhs);
}

void Substring::resolve() const
{
    auto source_view = m_source_string->utf16_string_view().substring_view(m_code_unit_offset, m_code_unit_length);

    m_utf16_string = Utf16String::from_utf16(source_view);
    m_deferred_kind = DeferredKind::None;
    m_source_string = nullptr;
}

Substring::Substring(GC::Ref<PrimitiveString> source_string, size_t code_unit_offset, size_t code_unit_length)
    : PrimitiveString(DeferredKind::Substring)
    , m_source_string(source_string)
    , m_code_unit_offset(code_unit_offset)
    , m_code_unit_length(code_unit_length)
{
}

Substring::~Substring() = default;

void Substring::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_source_string);
}

}
