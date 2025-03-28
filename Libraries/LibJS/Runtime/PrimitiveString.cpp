/*
 * Copyright (c) 2020-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/FlyString.h>
#include <AK/StringBuilder.h>
#include <AK/Utf16View.h>
#include <AK/Utf8View.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/PropertyKey.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

GC_DEFINE_ALLOCATOR(PrimitiveString);

PrimitiveString::PrimitiveString(PrimitiveString& lhs, PrimitiveString& rhs)
    : m_is_rope(true)
    , m_lhs(&lhs)
    , m_rhs(&rhs)
{
}

PrimitiveString::PrimitiveString(String string)
    : m_utf8_string(move(string))
{
}

PrimitiveString::PrimitiveString(Utf16String string)
    : m_utf16_string(move(string))
{
}

PrimitiveString::~PrimitiveString()
{
    if (has_utf8_string())
        vm().string_cache().remove(*m_utf8_string);
    if (has_utf16_string())
        vm().utf16_string_cache().remove(*m_utf16_string);
}

void PrimitiveString::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    if (m_is_rope) {
        visitor.visit(m_lhs);
        visitor.visit(m_rhs);
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
    (void)utf8_string();
    return m_utf8_string->bytes_as_string_view();
}

Utf16String PrimitiveString::utf16_string() const
{
    resolve_rope_if_needed(EncodingPreference::UTF16);

    if (!has_utf16_string()) {
        VERIFY(has_utf8_string());
        m_utf16_string = Utf16String::create(m_utf8_string->bytes_as_string_view());
    }

    return *m_utf16_string;
}

Utf16View PrimitiveString::utf16_string_view() const
{
    (void)utf16_string();
    return m_utf16_string->view();
}

ThrowCompletionOr<Optional<Value>> PrimitiveString::get(VM& vm, PropertyKey const& property_key) const
{
    if (property_key.is_symbol())
        return Optional<Value> {};
    if (property_key.is_string()) {
        if (property_key.as_string() == vm.names.length.as_string()) {
            auto length = utf16_string().length_in_code_units();
            return Value(static_cast<double>(length));
        }
    }
    auto index = canonical_numeric_index_string(property_key, CanonicalIndexMode::IgnoreNumericRoundtrip);
    if (!index.is_index())
        return Optional<Value> {};
    auto str = utf16_string_view();
    auto length = str.length_in_code_units();
    if (length <= index.as_index())
        return Optional<Value> {};
    return create(vm, Utf16String::create(str.substring_view(index.as_index(), 1)));
}

GC::Ref<PrimitiveString> PrimitiveString::create(VM& vm, Utf16String string)
{
    if (string.is_empty())
        return vm.empty_string();

    if (string.length_in_code_units() == 1) {
        u16 code_unit = string.code_unit_at(0);
        if (is_ascii(code_unit))
            return vm.single_ascii_character_string(static_cast<u8>(code_unit));
    }

    auto& string_cache = vm.utf16_string_cache();
    if (auto it = string_cache.find(string); it != string_cache.end())
        return *it->value;

    auto new_string = vm.heap().allocate<PrimitiveString>(string);
    string_cache.set(move(string), new_string);
    return *new_string;
}

GC::Ref<PrimitiveString> PrimitiveString::create(VM& vm, String string)
{
    if (string.is_empty())
        return vm.empty_string();

    if (auto bytes = string.bytes_as_string_view(); bytes.length() == 1) {
        auto ch = static_cast<u8>(bytes[0]);
        if (is_ascii(ch))
            return vm.single_ascii_character_string(ch);
    }

    auto& string_cache = vm.string_cache();
    if (auto it = string_cache.find(string); it != string_cache.end())
        return *it->value;

    auto new_string = vm.heap().allocate<PrimitiveString>(string);
    string_cache.set(move(string), new_string);
    return *new_string;
}

GC::Ref<PrimitiveString> PrimitiveString::create(VM& vm, FlyString const& string)
{
    return create(vm, string.to_string());
}

GC::Ref<PrimitiveString> PrimitiveString::create(VM& vm, StringView string)
{
    return create(vm, String::from_utf8(string).release_value());
}

GC::Ref<PrimitiveString> PrimitiveString::create(VM& vm, PrimitiveString& lhs, PrimitiveString& rhs)
{
    // We're here to concatenate two strings into a new rope string.
    // However, if any of them are empty, no rope is required.

    bool lhs_empty = lhs.is_empty();
    bool rhs_empty = rhs.is_empty();

    if (lhs_empty && rhs_empty)
        return vm.empty_string();

    if (lhs_empty)
        return rhs;

    if (rhs_empty)
        return lhs;

    return vm.heap().allocate<PrimitiveString>(lhs, rhs);
}

void PrimitiveString::resolve_rope_if_needed(EncodingPreference preference) const
{
    if (!m_is_rope)
        return;

    // This vector will hold all the pieces of the rope that need to be assembled
    // into the resolved string.
    Vector<PrimitiveString const*> pieces;
    size_t approximate_length = 0;

    // NOTE: We traverse the rope tree without using recursion, since we'd run out of
    //       stack space quickly when handling a long sequence of unresolved concatenations.
    Vector<PrimitiveString const*> stack;
    stack.append(m_rhs);
    stack.append(m_lhs);
    while (!stack.is_empty()) {
        auto const* current = stack.take_last();
        if (current->m_is_rope) {
            stack.append(current->m_rhs);
            stack.append(current->m_lhs);
            continue;
        }

        if (current->has_utf8_string())
            approximate_length += current->utf8_string_view().length();
        pieces.append(current);
    }

    if (preference == EncodingPreference::UTF16) {
        // The caller wants a UTF-16 string, so we can simply concatenate all the pieces
        // into a UTF-16 code unit buffer and create a Utf16String from it.

        Utf16Data code_units;
        for (auto const* current : pieces)
            code_units.extend(current->utf16_string().string());

        m_utf16_string = Utf16String::create(move(code_units));
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

        if (!Utf16View::is_high_surrogate(high_surrogate) || !Utf16View::is_low_surrogate(low_surrogate)) {
            builder.append(current_string_as_utf8);
            previous = current;
            continue;
        }

        // Remove 3 bytes from the builder and replace them with the UTF-8 encoded code point.
        builder.trim(3);
        builder.append_code_point(Utf16View::decode_surrogate_pair(high_surrogate, low_surrogate));

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

}
