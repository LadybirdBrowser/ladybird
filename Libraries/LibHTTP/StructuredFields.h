/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Concepts.h>
#include <AK/Error.h>
#include <AK/HashMap.h>
#include <AK/String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>

// https://www.rfc-editor.org/rfc/rfc9651.html
namespace HTTP::StructuredFields {

// https://www.rfc-editor.org/rfc/rfc9651.html#name-tokens
struct Token {
    ByteString value;

    bool operator==(Token const&) const = default;
};

// https://www.rfc-editor.org/rfc/rfc9651.html#name-dates
struct Date {
    i64 value { 0 };

    bool operator==(Date const&) const = default;
};

// https://www.rfc-editor.org/rfc/rfc9651.html#name-display-strings
struct DisplayString {
    String value;

    bool operator==(DisplayString const&) const = default;
};

// https://www.rfc-editor.org/rfc/rfc9651.html#name-items
// The alternatives are an Integer, a Decimal, a String, a Token, a Byte Sequence, a Boolean, a Date and a Display String.
using BareItem = Variant<i64, double, ByteString, Token, ByteBuffer, bool, Date, DisplayString>;

// https://www.rfc-editor.org/rfc/rfc9651.html#name-parameters
using Parameters = OrderedHashMap<ByteString, BareItem>;

struct Item {
    BareItem bare_item;
    Parameters parameters;
};

// https://www.rfc-editor.org/rfc/rfc9651.html#name-inner-lists
struct InnerList {
    Vector<Item> items;
    Parameters parameters;
};

using ItemOrInnerList = Variant<Item, InnerList>;

// https://www.rfc-editor.org/rfc/rfc9651.html#name-lists
using List = Vector<ItemOrInnerList>;

// https://www.rfc-editor.org/rfc/rfc9651.html#name-dictionaries
using Dictionary = OrderedHashMap<ByteString, ItemOrInnerList>;

template<typename T>
concept FieldType = OneOf<T, List, Dictionary, Item>;

template<FieldType T>
ErrorOr<T> parse(StringView input_bytes);

}
