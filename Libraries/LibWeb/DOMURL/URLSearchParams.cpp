/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2023-2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <AK/StringBuilder.h>
#include <AK/Utf8View.h>
#include <LibGC/Heap.h>
#include <LibURL/Parser.h>
#include <LibURL/RustFFI.h>
#include <LibURL/RustIntegration.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/DOMURL/URLSearchParams.h>

namespace Web::DOMURL {

GC_DEFINE_ALLOCATOR(URLSearchParams);

URLSearchParams::URLSearchParams(Vector<QueryParam> list)
    : m_list(move(list))
{
}

URLSearchParams::~URLSearchParams() = default;

void URLSearchParams::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_url);
}

// https://url.spec.whatwg.org/#concept-urlencoded-serializer
String url_encode(Vector<QueryParam> const& tuples, StringView encoding)
{
    Vector<URL::FFI::FfiFormTuple> ffi_tuples;
    ffi_tuples.ensure_capacity(tuples.size());
    for (auto const& tuple : tuples)
        ffi_tuples.unchecked_append({ URL::RustIntegration::rust_url_input(tuple.name.utf16_view()), URL::RustIntegration::rust_url_input(tuple.value.utf16_view()) });

    String output;
    URL::FFI::rust_url_form_urlencoded_serialize(ffi_tuples.data(), ffi_tuples.size(), { reinterpret_cast<u8 const*>(encoding.characters_without_null_termination()), encoding.length() }, &output, [](void* ctx, URL::FFI::RustUrlByteSlice serialization) {
        *static_cast<String*>(ctx) = String::from_ascii_without_validation({ serialization.data, serialization.length });
    });
    return output;
}

// https://url.spec.whatwg.org/#concept-urlencoded-parser
Vector<QueryParam> url_decode(StringView input)
{
    Vector<QueryParam> output;
    URL::FFI::rust_url_form_urlencoded_parse({ reinterpret_cast<u8 const*>(input.characters_without_null_termination()), input.length() }, &output, [](void* ctx, URL::FFI::RustUrlByteSlice name, URL::FFI::RustUrlByteSlice value) {
        auto utf16_from_ffi = [](URL::FFI::RustUrlByteSlice string) {
            return Utf16String::from_utf8({ reinterpret_cast<char const*>(string.data), string.length });
        };
        static_cast<Vector<QueryParam>*>(ctx)->empend(utf16_from_ffi(name), utf16_from_ffi(value));
    });
    return output;
}

Vector<QueryParam> url_decode(Utf16View input)
{
    auto utf8 = input.to_utf8_but_should_be_ported_to_utf16();
    return url_decode(utf8);
}

GC::Ref<URLSearchParams> URLSearchParams::create(Vector<QueryParam> list)
{
    return GC::Heap::the().allocate<URLSearchParams>(move(list));
}

// https://url.spec.whatwg.org/#urlsearchparams-initialize
GC::Ref<URLSearchParams> URLSearchParams::create(StringView init)
{
    // NOTE: We skip the other steps since we know it is a string at this point.
    // b. Set query’s list to the result of parsing init.
    return URLSearchParams::create(url_decode(init));
}

GC::Ref<URLSearchParams> URLSearchParams::create(Utf16View init)
{
    return URLSearchParams::create(url_decode(init));
}

// https://url.spec.whatwg.org/#dom-urlsearchparams-urlsearchparams
// https://url.spec.whatwg.org/#urlsearchparams-initialize
WebIDL::ExceptionOr<GC::Ref<URLSearchParams>> URLSearchParams::create_from_init(Variant<Vector<Vector<Utf16String>>, OrderedHashMap<Utf16String, Utf16String>, Utf16String> const& init)
{
    // 1. If init is a string and starts with U+003F (?), then remove the first code point from init.
    // NOTE: We do this when we know that it's a string on step 3 of initialization.

    // 2. Initialize this with init.

    // URLSearchParams init from this point forward

    // 1. If init is a sequence, then for each pair in init:
    if (init.has<Vector<Vector<Utf16String>>>()) {
        auto const& init_sequence = init.get<Vector<Vector<Utf16String>>>();

        Vector<QueryParam> list;
        list.ensure_capacity(init_sequence.size());

        for (auto const& pair : init_sequence) {
            // a. If pair does not contain exactly two items, then throw a TypeError.
            if (pair.size() != 2)
                return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Expected each pair to contain exactly 2 items"_utf16 };

            // b. Append a new name-value pair whose name is pair’s first item, and value is pair’s second item, to query’s list.
            list.append(QueryParam { .name = pair[0], .value = pair[1] });
        }

        return URLSearchParams::create(move(list));
    }

    // 2. Otherwise, if init is a record, then for each name → value of init, append a new name-value pair whose name is name and value is value, to query’s list.
    if (init.has<OrderedHashMap<Utf16String, Utf16String>>()) {
        auto const& init_record = init.get<OrderedHashMap<Utf16String, Utf16String>>();

        Vector<QueryParam> list;
        list.ensure_capacity(init_record.size());

        for (auto const& pair : init_record)
            list.append(QueryParam { .name = pair.key, .value = pair.value });

        return URLSearchParams::create(move(list));
    }

    // 3. Otherwise:
    // a. Assert: init is a string.
    // NOTE: `get` performs `VERIFY(has<T>())`
    auto const& init_string = init.get<Utf16String>();

    // See NOTE at the start of this function.
    auto init_string_view = init_string.utf16_view();
    auto stripped_init = init_string_view.substring_view(init_string_view.starts_with('?'));

    // b. Set query’s list to the result of parsing init.
    return URLSearchParams::create(stripped_init);
}

// https://url.spec.whatwg.org/#dom-urlsearchparams-size
size_t URLSearchParams::size() const
{
    // The size getter steps are to return this’s list’s size.
    return m_list.size();
}

// https://url.spec.whatwg.org/#dom-urlsearchparams-append
void URLSearchParams::append(Utf16String const& name, Utf16String const& value)
{
    // 1. Append a new name-value pair whose name is name and value is value, to list.
    m_list.empend(name, value);

    // 2. Update this.
    update();
}

// https://url.spec.whatwg.org/#concept-urlsearchparams-update
void URLSearchParams::update()
{
    // 1. If query’s URL object is null, then return.
    if (!m_url)
        return;

    // 2. Let serializedQuery be the serialization of query’s list.
    Optional<String> serialized_query = serialize_to_byte_string();

    // 3. If serializedQuery is the empty string, then set serializedQuery to null.
    if (serialized_query == String {})
        serialized_query = {};

    // 4. Set query’s URL object’s URL’s query to serializedQuery.
    m_url->set_query({}, serialized_query.map([](String const& query) { return query.bytes_as_string_view(); }));
}

// https://url.spec.whatwg.org/#dom-urlsearchparams-delete
void URLSearchParams::delete_(Utf16String const& name, Optional<Utf16String> const& value)
{
    // 1. If value is given, then remove all tuples whose name is name and value is value from this’s list.
    if (value.has_value()) {
        m_list.remove_all_matching([&name, &value](auto& entry) {
            return entry.name == name && entry.value == value.value();
        });
    }
    // 2. Otherwise, remove all tuples whose name is name from this’s list.
    else {
        m_list.remove_all_matching([&name](auto& entry) {
            return entry.name == name;
        });
    }

    // 2. Update this.
    update();
}

Optional<Utf16String> URLSearchParams::get(Utf16String const& name)
{
    // return the value of the first name-value pair whose name is name in this’s list, if there is such a pair, and null otherwise.
    auto result = m_list.find_if([&name](auto& entry) {
        return entry.name == name;
    });
    if (result.is_end())
        return {};
    return result->value;
}

// https://url.spec.whatwg.org/#dom-urlsearchparams-getall
Vector<Utf16String> URLSearchParams::get_all(Utf16String const& name)
{
    // return the values of all name-value pairs whose name is name, in this’s list, in list order, and the empty sequence otherwise.
    Vector<Utf16String> values;
    for (auto& entry : m_list) {
        if (entry.name == name)
            values.append(entry.value);
    }
    return values;
}

// https://url.spec.whatwg.org/#dom-urlsearchparams-has
bool URLSearchParams::has(Utf16String const& name, Optional<Utf16String> const& value)
{
    // 1. If value is given and there is a tuple whose name is name and value is value in this’s list, then return true.
    if (value.has_value()) {
        if (!m_list.find_if([&name, &value](auto& entry) {
                       return entry.name == name && entry.value == value.value();
                   })
                .is_end()) {
            return true;
        }
    }
    // 2. If value is not given and there is a tuple whose name is name in this’s list, then return true.
    else {
        if (!m_list.find_if([&name](auto& entry) {
                       return entry.name == name;
                   })
                .is_end()) {
            return true;
        }
    }

    // 3. Return false.
    return false;
}

void URLSearchParams::set(Utf16String const& name, Utf16String const& value)
{
    // 1. If this’s list contains any name-value pairs whose name is name, then set the value of the first such name-value pair to value and remove the others.
    auto existing = m_list.find_if([&name](auto& entry) {
        return entry.name == name;
    });
    if (!existing.is_end()) {
        existing->value = value;
        m_list.remove_all_matching([&name, &existing](auto& entry) {
            return &entry != &*existing && entry.name == name;
        });
    }
    // 2. Otherwise, append a new name-value pair whose name is name and value is value, to this’s list.
    else {
        m_list.empend(name, value);
    }

    // 3. Update this.
    update();
}

// https://url.spec.whatwg.org/#dom-urlsearchparams-sort
void URLSearchParams::sort()
{
    // 1. Set this’s list to the result of sorting in ascending order this’s list, with a being less than b if a’s name is code unit less than b’s name.
    insertion_sort(m_list, [](auto& a, auto& b) {
        return a.name.utf16_view().is_code_unit_less_than(b.name);
    });

    // 2. Update this.
    update();
}

String URLSearchParams::serialize_to_byte_string() const
{
    // return the serialization of this’s list.
    return url_encode(m_list);
}

Utf16String URLSearchParams::to_string() const
{
    auto serialized = serialize_to_byte_string();
    return Utf16String::from_ascii_without_validation(serialized.bytes());
}

void URLSearchParams::for_each(ForEachCallback callback)
{
    for (auto i = 0u; i < m_list.size(); ++i) {
        auto& query_param = m_list[i]; // We are explicitly iterating over the indices here as the callback might delete items from the list
        if (callback(query_param.name, query_param.value) == IterationDecision::Break)
            break;
    }
}

}
