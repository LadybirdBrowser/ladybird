/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <LibJS/Runtime/Shape.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

struct ValueAndAttributes {
    Value value;
    PropertyAttributes attributes { default_attributes };

    Optional<u32> property_offset {};
};

class IndexedProperties;
class IndexedPropertyIterator;
class GenericIndexedPropertyStorage;

class IndexedPropertyStorage {
public:
    virtual ~IndexedPropertyStorage() = default;

    enum class IsSimpleStorage {
        No,
        Yes,
    };

    virtual bool has_index(u32 index) const = 0;
    virtual Optional<ValueAndAttributes> get(u32 index) const = 0;
    virtual void put(u32 index, Value value, PropertyAttributes attributes = default_attributes) = 0;
    virtual void remove(u32 index) = 0;

    virtual ValueAndAttributes take_first() = 0;
    virtual ValueAndAttributes take_last() = 0;

    virtual size_t size() const = 0;
    virtual size_t array_like_size() const = 0;
    virtual bool set_array_like_size(size_t new_size) = 0;

    bool is_simple_storage() const { return m_is_simple_storage; }

protected:
    explicit IndexedPropertyStorage(IsSimpleStorage is_simple_storage)
        : m_is_simple_storage(is_simple_storage == IsSimpleStorage::Yes) {};

private:
    bool m_is_simple_storage { false };
};

class SimpleIndexedPropertyStorage final : public IndexedPropertyStorage {
public:
    SimpleIndexedPropertyStorage()
        : IndexedPropertyStorage(IsSimpleStorage::Yes) {};
    explicit SimpleIndexedPropertyStorage(Vector<Value>&& initial_values);

    virtual bool has_index(u32 index) const override;
    virtual Optional<ValueAndAttributes> get(u32 index) const override;
    virtual void put(u32 index, Value value, PropertyAttributes attributes = default_attributes) override;
    virtual void remove(u32 index) override;

    virtual ValueAndAttributes take_first() override;
    virtual ValueAndAttributes take_last() override;

    virtual size_t size() const override { return m_packed_elements.size(); }
    virtual size_t array_like_size() const override { return m_array_size; }
    virtual bool set_array_like_size(size_t new_size) override;

    Vector<Value> const& elements() const { return m_packed_elements; }

    [[nodiscard]] bool inline_has_index(u32 index) const
    {
        return index < m_array_size && !m_packed_elements.data()[index].is_empty();
    }

    [[nodiscard]] Optional<ValueAndAttributes> inline_get(u32 index) const
    {
        if (!inline_has_index(index))
            return {};
        return ValueAndAttributes { m_packed_elements.data()[index], default_attributes };
    }

private:
    friend GenericIndexedPropertyStorage;

    void grow_storage_if_needed();

    size_t m_array_size { 0 };
    Vector<Value> m_packed_elements;
};

class GenericIndexedPropertyStorage final : public IndexedPropertyStorage {
public:
    explicit GenericIndexedPropertyStorage(SimpleIndexedPropertyStorage&&);
    explicit GenericIndexedPropertyStorage()
        : IndexedPropertyStorage(IsSimpleStorage::No) {};

    virtual bool has_index(u32 index) const override;
    virtual Optional<ValueAndAttributes> get(u32 index) const override;
    virtual void put(u32 index, Value value, PropertyAttributes attributes = default_attributes) override;
    virtual void remove(u32 index) override;

    virtual ValueAndAttributes take_first() override;
    virtual ValueAndAttributes take_last() override;

    virtual size_t size() const override { return m_sparse_elements.size(); }
    virtual size_t array_like_size() const override { return m_array_size; }
    virtual bool set_array_like_size(size_t new_size) override;

    HashMap<u32, ValueAndAttributes> const& sparse_elements() const { return m_sparse_elements; }

private:
    size_t m_array_size { 0 };
    HashMap<u32, ValueAndAttributes> m_sparse_elements;
};

class IndexedPropertyIterator {
public:
    IndexedPropertyIterator(IndexedProperties const&, u32 starting_index, bool skip_empty);

    IndexedPropertyIterator& operator++();
    IndexedPropertyIterator& operator*();
    bool operator!=(IndexedPropertyIterator const&) const;

    u32 index() const { return m_index; }

private:
    void skip_empty_indices();

    IndexedProperties const& m_indexed_properties;
    Vector<u32> m_cached_indices;
    size_t m_next_cached_index { 0 };
    u32 m_index { 0 };
    bool m_skip_empty { false };
};

class IndexedProperties {
public:
    IndexedProperties() = default;

    explicit IndexedProperties(Vector<Value> values)
    {
        if (!values.is_empty())
            m_storage = make<SimpleIndexedPropertyStorage>(move(values));
    }

    bool has_index(u32 index) const { return m_storage ? m_storage->has_index(index) : false; }
    Optional<ValueAndAttributes> get(u32 index) const;
    void put(u32 index, Value value, PropertyAttributes attributes = default_attributes);
    void remove(u32 index);

    void append(Value value, PropertyAttributes attributes = default_attributes) { put(array_like_size(), value, attributes); }

    IndexedPropertyIterator begin(bool skip_empty = true) const { return IndexedPropertyIterator(*this, 0, skip_empty); }
    IndexedPropertyIterator end() const { return IndexedPropertyIterator(*this, array_like_size(), false); }

    bool is_empty() const { return array_like_size() == 0; }
    size_t array_like_size() const { return m_storage ? m_storage->array_like_size() : 0; }
    bool set_array_like_size(size_t);

    IndexedPropertyStorage* storage() { return m_storage; }
    IndexedPropertyStorage const* storage() const { return m_storage; }

    size_t real_size() const;

    Vector<u32> indices() const;

    template<typename Callback>
    void for_each_value(Callback callback)
    {
        if (!m_storage)
            return;
        if (m_storage->is_simple_storage()) {
            for (auto& value : static_cast<SimpleIndexedPropertyStorage&>(*m_storage).elements())
                callback(value);
        } else {
            for (auto& element : static_cast<GenericIndexedPropertyStorage const&>(*m_storage).sparse_elements())
                callback(element.value.value);
        }
    }

private:
    void switch_to_generic_storage();
    void ensure_storage();

    OwnPtr<IndexedPropertyStorage> m_storage;
};

}
