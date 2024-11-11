/*
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>

namespace Web::IndexedDB {

// https://w3c.github.io/IndexedDB/#key-construct
class Key {
    // A key has an associated type which is one of: number, date, string, binary, or array.
    enum KeyType {
        Number,
        Date,
        String,
        Binary,
        Array,
    };

    // A key also has an associated value, which will be either:
    // * an unrestricted double if type is number or date,
    // * a DOMString if type is string,
    // * a byte sequence if type is binary,
    // * a list of other keys if type is array.
    using KeyValue = Variant<double, AK::String, ByteBuffer, Vector<Key>>;

public:
    [[nodiscard]] KeyType type() { return m_type; }
    [[nodiscard]] KeyValue value() { return m_value; }

    [[nodiscard]] static Key create_number(double value) { return Key(Number, value); }
    [[nodiscard]] static Key create_date(double value) { return Key(Date, value); }
    [[nodiscard]] static Key create_string(AK::String const& value) { return Key(String, value); }
    [[nodiscard]] static Key create_binary(ByteBuffer const& value) { return Key(Binary, value); }
    [[nodiscard]] static Key create_array(Vector<Key> const& value) { return Key(Array, value); }

    [[nodiscard]] static i8 compare_two_keys(Key a, Key b);

private:
    Key(KeyType type, KeyValue value)
        : m_type(type)
        , m_value(value)
    {
    }

    KeyType m_type;
    KeyValue m_value;
};

}
