/*
 * Copyright (c) 2020-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/FlyString.h>
#include <AK/StringBuilder.h>
#include <AK/UnicodeUtils.h>
#include <AK/Utf16FlyString.h>
#include <AK/Utf16View.h>
#include <AK/Utf8View.h>
#include <LibJS/Runtime/AbstractOperations.h>
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

GC::Ref<PrimitiveString> PrimitiveString::create(VM& vm, String const& string)
{
    if (string.is_empty())
        return vm.empty_string();

    auto const length_in_code_units = string.length_in_code_units();

    if (length_in_code_units == 1) {
        auto bytes = string.bytes();
        if (auto ch = bytes[0]; is_ascii(ch))
            return vm.single_ascii_character_string(ch);
    }

    if (string.length_in_code_units() > MAX_LENGTH_FOR_STRING_CACHE) {
        return vm.heap().allocate<PrimitiveString>(string);
    }

    auto& string_cache = vm.string_cache();
    if (auto it = string_cache.find(string); it != string_cache.end())
        return *it->value;

    auto new_string = vm.heap().allocate<PrimitiveString>(string);
    string_cache.set(move(string), new_string);
    return *new_string;
}

GC::Ref<PrimitiveString> PrimitiveString::create(VM& vm, StringView string)
{
    return create(vm, String::from_utf8(string).release_value());
}

GC::Ref<PrimitiveString> PrimitiveString::create(VM& vm, FlyString const& string)
{
    return create(vm, string.to_string());
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

    return vm.heap().allocate<RopeString>(lhs, rhs);
}

PrimitiveString::PrimitiveString(Utf16String string)
    : m_utf16_string(move(string))
{
}

PrimitiveString::PrimitiveString(String string)
    : m_utf8_string(move(string))
{
}

PrimitiveString::~PrimitiveString() = default;

void PrimitiveString::finalize()
{
    Base::finalize();
    if (has_utf16_string()) {
        auto const& string = *m_utf16_string;
        if (string.length_in_code_units() <= MAX_LENGTH_FOR_STRING_CACHE)
            vm().utf16_string_cache().remove(string);
    }
    if (has_utf8_string()) {
        auto const& string = *m_utf8_string;
        if (string.length_in_code_units() <= MAX_LENGTH_FOR_STRING_CACHE)
            vm().string_cache().remove(*m_utf8_string);
    }
}

bool PrimitiveString::is_empty() const
{
    if (m_is_rope) {
        // NOTE: We never make an empty rope string.
        return false;
    }

    if (has_utf16_string())
        return m_utf16_string->is_empty();
    if (has_utf8_string())
        return m_utf8_string->is_empty();
    VERIFY_NOT_REACHED();
}

String PrimitiveString::utf8_string() const
{
    resolve_rope_if_needed(EncodingPreference::UTF8);

    if (!has_utf8_string()) {
        VERIFY(has_utf16_string());
        m_utf8_string = m_utf16_string->to_utf8();
    }

    return *m_utf8_string;
}

StringView PrimitiveString::utf8_string_view() const
{
    if (!has_utf8_string())
        (void)utf8_string();
    return m_utf8_string->bytes_as_string_view();
}

Utf16String PrimitiveString::utf16_string() const
{
    resolve_rope_if_needed(EncodingPreference::UTF16);

    if (!has_utf16_string()) {
        VERIFY(has_utf8_string());
        m_utf16_string = Utf16String::from_utf8(*m_utf8_string);
    }

    return *m_utf16_string;
}

Utf16View PrimitiveString::utf16_string_view() const
{
    if (!has_utf16_string())
        (void)utf16_string();
    return *m_utf16_string;
}

size_t PrimitiveString::length_in_utf16_code_units() const
{
    return utf16_string_view().length_in_code_units();
}

bool PrimitiveString::operator==(PrimitiveString const& other) const
{
    if (this == &other)
        return true;
    if (m_utf8_string.has_value() && other.m_utf8_string.has_value())
        return m_utf8_string->bytes_as_string_view() == other.m_utf8_string->bytes_as_string_view();
    if (m_utf16_string.has_value() && other.m_utf16_string.has_value())
        return *m_utf16_string == *other.m_utf16_string;
    return utf8_string_view() == other.utf8_string_view();
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

    return create(vm, string.substring_view(index.as_index(), 1));
}

void PrimitiveString::resolve_rope_if_needed(EncodingPreference preference) const
{
    if (!m_is_rope)
        return;

    auto const& rope_string = static_cast<RopeString const&>(*this);
    rope_string.resolve(preference);
}

void RopeString::resolve(EncodingPreference preference) const
{

    // This vector will hold all the pieces of the rope that need to be assembled
    // into the resolved string.
    Vector<PrimitiveString const*, 2> pieces;
    size_t approximate_length = 0;
    size_t length_in_utf16_code_units = 0;

    // NOTE: We traverse the rope tree without using recursion, since we'd run out of
    //       stack space quickly when handling a long sequence of unresolved concatenations.
    Vector<PrimitiveString const*, 2> stack;
    stack.append(m_rhs);
    stack.append(m_lhs);
    while (!stack.is_empty()) {
        auto const* current = stack.take_last();
        if (current->m_is_rope) {
            auto& current_rope_string = static_cast<RopeString const&>(*current);
            stack.append(current_rope_string.m_rhs);
            stack.append(current_rope_string.m_lhs);
            continue;
        }

        if (current->has_utf8_string())
            approximate_length += current->utf8_string_view().length();
        if (preference == EncodingPreference::UTF16)
            length_in_utf16_code_units += current->length_in_utf16_code_units();
        pieces.append(current);
    }

    if (preference == EncodingPreference::UTF16) {
        // The caller wants a UTF-16 string, so we can simply concatenate all the pieces
        // into a UTF-16 code unit buffer and create a Utf16String from it.
        StringBuilder builder(StringBuilder::Mode::UTF16, length_in_utf16_code_units);

        for (auto const* current : pieces) {
            if (current->has_utf16_string())
                builder.append(current->utf16_string_view());
            else
                builder.append(current->utf8_string_view());
        }

        m_utf16_string = builder.to_utf16_string();
        m_is_rope = false;
        m_lhs = nullptr;
        m_rhs = nullptr;
        return;
    }

    // Now that we have all the pieces, we can concatenate them using a StringBuilder.
    StringBuilder builder(approximate_length);

    // We keep track of the previous piece in order to handle surrogate pairs spread across two pieces.
    PrimitiveString const* previous = nullptr;
    for (auto const* current : pieces) {
        if (!previous) {
            // This is the very first piece, just append it and continue.
            builder.append(current->utf8_string());
            previous = current;
            continue;
        }

        // Get the UTF-8 representations for both strings.
        auto current_string_as_utf8 = current->utf8_string_view();
        auto previous_string_as_utf8 = previous->utf8_string_view();

        // NOTE: Now we need to look at the end of the previous string and the start
        //       of the current string, to see if they should be combined into a surrogate.

        // Surrogates encoded as UTF-8 are 3 bytes.
        if ((previous_string_as_utf8.length() < 3) || (current_string_as_utf8.length() < 3)) {
            builder.append(current_string_as_utf8);
            previous = current;
            continue;
        }

        // Might the previous string end with a UTF-8 encoded surrogate?
        if ((static_cast<u8>(previous_string_as_utf8[previous_string_as_utf8.length() - 3]) & 0xf0) != 0xe0) {
            // If not, just append the current string and continue.
            builder.append(current_string_as_utf8);
            previous = current;
            continue;
        }

        // Might the current string begin with a UTF-8 encoded surrogate?
        if ((static_cast<u8>(current_string_as_utf8[0]) & 0xf0) != 0xe0) {
            // If not, just append the current string and continue.
            builder.append(current_string_as_utf8);
            previous = current;
            continue;
        }

        auto high_surrogate = *Utf8View(previous_string_as_utf8.substring_view(previous_string_as_utf8.length() - 3)).begin();
        auto low_surrogate = *Utf8View(current_string_as_utf8).begin();

        if (!AK::UnicodeUtils::is_utf16_high_surrogate(high_surrogate) || !AK::UnicodeUtils::is_utf16_low_surrogate(low_surrogate)) {
            builder.append(current_string_as_utf8);
            previous = current;
            continue;
        }

        // Remove 3 bytes from the builder and replace them with the UTF-8 encoded code point.
        builder.trim(3);
        builder.append_code_point(AK::UnicodeUtils::decode_utf16_surrogate_pair(high_surrogate, low_surrogate));

        // Append the remaining part of the current string.
        builder.append(current_string_as_utf8.substring_view(3));
        previous = current;
    }

    // NOTE: We've already produced valid UTF-8 above, so there's no need for additional validation.
    m_utf8_string = builder.to_string_without_validation();
    m_is_rope = false;
    m_lhs = nullptr;
    m_rhs = nullptr;
}

RopeString::RopeString(GC::Ref<PrimitiveString> lhs, GC::Ref<PrimitiveString> rhs)
    : PrimitiveString(RopeTag::Rope)
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

}
