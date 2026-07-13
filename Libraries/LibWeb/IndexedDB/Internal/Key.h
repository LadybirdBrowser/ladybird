/*
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/String.h>
#include <AK/Utf16String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibGC/HeapVector.h>
#include <LibGC/Ptr.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::IndexedDB {

// https://w3c.github.io/IndexedDB/#key-construct
class Key : public JS::Cell {
    GC_CELL(Key, JS::Cell);
    GC_DECLARE_ALLOCATOR(Key);

public:
    // A key also has an associated value, which will be either:
    // * an unrestricted double if type is number or date,
    // * a DOMString if type is string,
    // * a byte sequence if type is binary,
    // * a list of other keys if type is array.
    using KeyValue = Variant<double, Utf16String, ByteBuffer, GC::Root<GC::HeapVector<GC::Ref<Key>>>>;
    using KeyValueInternal = Variant<double, Utf16String, ByteBuffer, GC::Ref<GC::HeapVector<GC::Ref<Key>>>>;

    // A key has an associated type which is one of: number, date, string, binary, or array.
    enum KeyType {
        Invalid,
        Number,
        Date,
        String,
        Binary,
        Array,
    };

    [[nodiscard]] static GC::Ref<Key> create(JS::Realm&, KeyType, KeyValue);
    virtual ~Key();

    [[nodiscard]] KeyType type() const { return m_type; }
    [[nodiscard]] KeyValue value();

    [[nodiscard]] bool is_invalid() const { return m_type == Invalid; }

    [[nodiscard]] double value_as_double() const { return m_value.get<double>(); }
    [[nodiscard]] Utf16String const& value_as_string() const { return m_value.get<Utf16String>(); }
    [[nodiscard]] ByteBuffer const& value_as_byte_buffer() const { return m_value.get<ByteBuffer>(); }
    [[nodiscard]] ReadonlySpan<GC::Ref<Key>> value_as_vector() const { return m_value.get<GC::Ref<GC::HeapVector<GC::Ref<Key>>>>()->elements(); }
    [[nodiscard]] ReadonlySpan<GC::Ref<Key>> subkeys() const
    {
        VERIFY(m_type == Array);
        return value_as_vector();
    }

    [[nodiscard]] static GC::Ref<Key> create_number(JS::Realm& realm, double value) { return create(realm, Number, value); }
    [[nodiscard]] static GC::Ref<Key> create_date(JS::Realm& realm, double value) { return create(realm, Date, value); }
    [[nodiscard]] static GC::Ref<Key> create_string(JS::Realm& realm, Utf16String const& value) { return create(realm, String, value); }
    [[nodiscard]] static GC::Ref<Key> create_binary(JS::Realm& realm, ByteBuffer const& value) { return create(realm, Binary, value); }
    [[nodiscard]] static GC::Ref<Key> create_array(JS::Realm& realm, GC::Root<GC::HeapVector<GC::Ref<Key>>> const& value) { return create(realm, Array, value); }
    [[nodiscard]] static GC::Ref<Key> create_invalid(JS::Realm& realm, Utf16String const& value) { return create(realm, Invalid, value); }

    [[nodiscard]] static i8 compare_two_keys(GC::Ref<Key> a, GC::Ref<Key> b);
    [[nodiscard]] static bool equals(GC::Ref<Key> a, GC::Ref<Key> b) { return compare_two_keys(a, b) == 0; }
    [[nodiscard]] static bool less_than(GC::Ref<Key> a, GC::Ref<Key> b) { return compare_two_keys(a, b) < 0; }
    [[nodiscard]] static bool greater_than(GC::Ref<Key> a, GC::Ref<Key> b) { return compare_two_keys(a, b) > 0; }
    [[nodiscard]] static bool less_than_or_equal(GC::Ref<Key> a, GC::Ref<Key> b) { return compare_two_keys(a, b) <= 0; }
    [[nodiscard]] static bool greater_than_or_equal(GC::Ref<Key> a, GC::Ref<Key> b) { return compare_two_keys(a, b) >= 0; }

    AK::String dump() const;

private:
    Key(KeyType, KeyValue);
    virtual void visit_edges(Visitor&) override;

    KeyType m_type;
    KeyValueInternal m_value;
};

}
