/*
 * Copyright (c) 2022, networkException <networkexception@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibURL/URL.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

// An ordered map kept sorted in descending code unit order of its keys.
template<typename T>
class DescendingCodeUnitOrderedMap {
public:
    struct Entry {
        Utf16String key;
        T value;
    };

    auto begin() const { return m_entries.begin(); }
    auto end() const { return m_entries.end(); }
    auto begin() { return m_entries.begin(); }
    auto end() { return m_entries.end(); }

    bool contains(Utf16View key) const { return index_of(key).has_value(); }

    T const* find(Utf16View key) const
    {
        auto index = index_of(key);
        return index.has_value() ? &m_entries[*index].value : nullptr;
    }

    T* find(Utf16View key)
    {
        auto index = index_of(key);
        return index.has_value() ? &m_entries[*index].value : nullptr;
    }

    void set(Utf16String key, T value)
    {
        auto index = lower_bound(key);
        if (index < m_entries.size() && m_entries[index].key == key)
            m_entries[index].value = move(value);
        else
            m_entries.insert(index, Entry { move(key), move(value) });
    }

    template<typename Predicate>
    void remove_all_matching(Predicate const& predicate)
    {
        m_entries.remove_all_matching([&](Entry const& entry) { return predicate(entry.key, entry.value); });
    }

private:
    Optional<size_t> index_of(Utf16View key) const
    {
        auto index = lower_bound(key);
        if (index < m_entries.size() && m_entries[index].key == key)
            return index;
        return {};
    }

    size_t lower_bound(Utf16View key) const
    {
        size_t low = 0;
        size_t high = m_entries.size();
        while (low < high) {
            auto middle = low + (high - low) / 2;
            if (key.is_code_unit_less_than(m_entries[middle].key))
                low = middle + 1;
            else
                high = middle;
        }
        return low;
    }

    Vector<Entry> m_entries;
};

using ModuleSpecifierMap = DescendingCodeUnitOrderedMap<Optional<URL::URL>>;
using ModuleScopesMap = DescendingCodeUnitOrderedMap<ModuleSpecifierMap>;
using ModuleIntegrityMap = HashMap<URL::URL, Utf16String>;

// https://html.spec.whatwg.org/multipage/webappapis.html#import-map
class ImportMap {
public:
    ImportMap() = default;

    ModuleSpecifierMap const& imports() const { return m_imports; }
    ModuleSpecifierMap& imports() { return m_imports; }
    void set_imports(ModuleSpecifierMap const& imports) { m_imports = imports; }

    ModuleScopesMap const& scopes() const { return m_scopes; }
    ModuleScopesMap& scopes() { return m_scopes; }
    void set_scopes(ModuleScopesMap const& scopes) { m_scopes = scopes; }

    ModuleIntegrityMap const& integrity() const { return m_integrity; }
    ModuleIntegrityMap integrity() { return m_integrity; }
    void set_integrity(ModuleIntegrityMap const& integrity) { m_integrity = integrity; }

private:
    ModuleSpecifierMap m_imports;
    ModuleScopesMap m_scopes;
    ModuleIntegrityMap m_integrity;
};

WebIDL::ExceptionOr<ImportMap> parse_import_map_string(JS::Realm& realm, Utf16View input, URL::URL base_url);
Optional<Utf16String> normalize_specifier_key(JS::Realm& realm, Utf16View specifier_key, URL::URL base_url);
WebIDL::ExceptionOr<ModuleSpecifierMap> sort_and_normalise_module_specifier_map(JS::Realm& realm, JS::Object& original_map, URL::URL base_url);
WebIDL::ExceptionOr<ModuleScopesMap> sort_and_normalise_scopes(JS::Realm& realm, JS::Object& original_map, URL::URL base_url);
WebIDL::ExceptionOr<ModuleIntegrityMap> normalize_module_integrity_map(JS::Realm& realm, JS::Object& original_map, URL::URL base_url);
void merge_existing_and_new_import_maps(Window&, ImportMap&);

}
