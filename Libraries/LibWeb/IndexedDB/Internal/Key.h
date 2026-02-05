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
#include <LibGC/Ptr.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::IndexedDB {

// https://w3c.github.io/IndexedDB/#key-construct
class Key : public JS::Cell {
    GC_CELL(Key, JS::Cell);
    GC_DECLARE_ALLOCATOR(Key);

    // A key also has an associated value, which will be either:
    // * an unrestricted double if type is number or date,
    // * a DOMString if type is string,
    // * a byte sequence if type is binary,
    // * a list of other keys if type is array.
    using KeyValue = Variant<double, AK::String, ByteBuffer, Vector<GC::Root<Key>>>;

    // A key has an associated type which is one of: number, date, string, binary, or array.
    enum KeyType {
        Invalid,
        Number,
        Date,
        String,
        Binary,
        Array,
    };

public:
    [[nodiscard]] static GC::Ref<Key> create(JS::Realm&, KeyType, KeyValue);
    virtual ~Key();

    [[nodiscard]] KeyType type() { return m_type; }
    [[nodiscard]] KeyValue value() { return m_value; }

    [[nodiscard]] bool is_invalid() { return m_type == Invalid; }

    [[nodiscard]] double value_as_double() { return m_value.get<double>(); }
    [[nodiscard]] AK::String value_as_string() { return m_value.get<AK::String>(); }
    [[nodiscard]] ByteBuffer value_as_byte_buffer() { return m_value.get<ByteBuffer>(); }
    [[nodiscard]] Vector<GC::Root<Key>> value_as_vector() { return m_value.get<Vector<GC::Root<Key>>>(); }
    [[nodiscard]] Vector<GC::Root<Key>> subkeys()
    {
        VERIFY(m_type == Array);
        return value_as_vector();
    }

    [[nodiscard]] static GC::Ref<Key> create_number(JS::Realm& realm, double value) { return create(realm, Number, value); }
    [[nodiscard]] static GC::Ref<Key> create_date(JS::Realm& realm, double value) { return create(realm, Date, value); }
    [[nodiscard]] static GC::Ref<Key> create_string(JS::Realm& realm, AK::String const& value) { return create(realm, String, value); }
    [[nodiscard]] static GC::Ref<Key> create_binary(JS::Realm& realm, ByteBuffer const& value) { return create(realm, Binary, value); }
    [[nodiscard]] static GC::Ref<Key> create_array(JS::Realm& realm, Vector<GC::Root<Key>> const& value) { return create(realm, Array, value); }
    [[nodiscard]] static GC::Ref<Key> create_invalid(JS::Realm& realm, AK::String const& value) { return create(realm, Invalid, value); }

    [[nodiscard]] static i8 compare_two_keys(GC::Ref<Key> a, GC::Ref<Key> b);
    [[nodiscard]] static bool equals(GC::Ref<Key> a, GC::Ref<Key> b) { return compare_two_keys(a, b) == 0; }
    [[nodiscard]] static bool less_than(GC::Ref<Key> a, GC::Ref<Key> b) { return compare_two_keys(a, b) < 0; }
    [[nodiscard]] static bool greater_than(GC::Ref<Key> a, GC::Ref<Key> b) { return compare_two_keys(a, b) > 0; }
    [[nodiscard]] static bool less_than_or_equal(GC::Ref<Key> a, GC::Ref<Key> b) { return compare_two_keys(a, b) <= 0; }
    [[nodiscard]] static bool greater_than_or_equal(GC::Ref<Key> a, GC::Ref<Key> b) { return compare_two_keys(a, b) >= 0; }

    AK::String dump() const;

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
