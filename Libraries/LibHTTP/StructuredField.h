/*
 * Copyright (c) 2026, Glenn Skrzypczak <glenn.skrzypczak@gmail.com>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/HashMap.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Variant.h>

namespace HTTP {

enum class StructuredFieldType : u8 {
    Dictionary,
    List,
    Item
};

// https://httpwg.org/specs/rfc9651.html#integer
struct StructuredFieldInteger {
    i64 value;
};

// https://httpwg.org/specs/rfc9651.html#decimal
struct StructuredFieldDecimal {
    double value;
};

// https://httpwg.org/specs/rfc9651.html#string
struct StructuredFieldString {
    String value;
};

// https://httpwg.org/specs/rfc9651.html#token
struct StructuredFieldToken {
    String value;
};

// https://httpwg.org/specs/rfc9651.html#binary
struct StructuredFieldByteSequence {
    ByteBuffer value;
};

// https://httpwg.org/specs/rfc9651.html#boolean
struct StructuredFieldBoolean {
    bool value;
};

// https://httpwg.org/specs/rfc9651.html#date
struct StructuredFieldDate {
    i64 value;
};

// https://httpwg.org/specs/rfc9651.html#displaystring
struct StructuredFieldDisplayString {
    String value;
};

using StructuredFieldBareItem = Variant<StructuredFieldInteger, StructuredFieldDecimal, StructuredFieldString, StructuredFieldToken, StructuredFieldByteSequence, StructuredFieldBoolean, StructuredFieldDate, StructuredFieldDisplayString>;

// https://httpwg.org/specs/rfc9651.html#param
using Parameters = HashMap<String, StructuredFieldBareItem>;

// https://httpwg.org/specs/rfc9651.html#item
struct StructuredFieldItem {
    StructuredFieldBareItem item;
    Parameters parameters;
};

// https://httpwg.org/specs/rfc9651.html#inner-list
struct StructuredFieldInnerList {
    Vector<StructuredFieldItem> members;
    Parameters parameters;
};

using StructuredFieldItemOrInnerList = Variant<StructuredFieldItem, StructuredFieldInnerList>;

// https://httpwg.org/specs/rfc9651.html#dictionary
struct StructuredFieldDictionary {
    HashMap<String, StructuredFieldItemOrInnerList> members;
};

// https://httpwg.org/specs/rfc9651.html#list
struct StructuredFieldList {
    Vector<StructuredFieldItemOrInnerList> members;
};

// https://httpwg.org/specs/rfc9651.html#rfc.section.2
using StructuredFieldValue = Variant<StructuredFieldList, StructuredFieldDictionary, StructuredFieldItem>;

ErrorOr<StructuredFieldValue> parse_structured_fields(StringView input_string, StructuredFieldType header_type);

}
