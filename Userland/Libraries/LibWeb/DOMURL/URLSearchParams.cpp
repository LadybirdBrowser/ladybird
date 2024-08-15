/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2023-2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <AK/StringBuilder.h>
#include <AK/Utf8View.h>
#include <LibTextCodec/Decoder.h>
#include <LibTextCodec/Encoder.h>
#include <LibURL/Parser.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/URLSearchParamsPrototype.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/DOMURL/URLSearchParams.h>

namespace Web::DOMURL {

JS_DEFINE_ALLOCATOR(URLSearchParams);

URLSearchParams::URLSearchParams(JS::Realm& realm, Vector<QueryParam> list)
    : PlatformObject(realm)
    , m_list(move(list))
{
}

URLSearchParams::~URLSearchParams() = default;

void URLSearchParams::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(URLSearchParams);
}

void URLSearchParams::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_url);
}

// https://url.spec.whatwg.org/#concept-urlencoded-serializer
// The application/x-www-form-urlencoded serializer takes a list of name-value tuples tuples, with an optional encoding encoding (default UTF-8), and then runs these steps. They return an ASCII string.
String url_encode(Vector<QueryParam> const& tuples, StringView encoding)
{
    // 1. Set encoding to the result of getting an output encoding from encoding.
    encoding = TextCodec::get_output_encoding(encoding);

    auto encoder = TextCodec::encoder_for(encoding);
    if (!encoder.has_value()) {
        // NOTE: Fallback to default utf-8 encoder.
        encoder = TextCodec::encoder_for("utf-8"sv);
    }

    // 2. Let output be the empty string.
    StringBuilder output;

    // 3. For each tuple of tuples:
    for (auto const& tuple : tuples) {
        // 1. Assert: tuple’s name and tuple’s value are scalar value strings.

        // 2. Let name be the result of running percent-encode after encoding with encoding, tuple’s name, the application/x-www-form-urlencoded percent-encode set, and true.
        auto name = URL::Parser::percent_encode_after_encoding(*encoder, tuple.name, URL::PercentEncodeSet::ApplicationXWWWFormUrlencoded, true);

        // 3. Let value be the result of running percent-encode after encoding with encoding, tuple’s value, the application/x-www-form-urlencoded percent-encode set, and true.
        auto value = URL::Parser::percent_encode_after_encoding(*encoder, tuple.value, URL::PercentEncodeSet::ApplicationXWWWFormUrlencoded, true);

        // 4. If output is not the empty string, then append U+0026 (&) to output.
        if (!output.is_empty())
            output.append('&');

        // 5. Append name, followed by U+003D (=), followed by value, to output.
        output.append(name);
        output.append('=');
        output.append(value);
    }

    // 4. Return output.
    return MUST(output.to_string());
}

// https://url.spec.whatwg.org/#concept-urlencoded-parser
// The application/x-www-form-urlencoded parser takes a byte sequence input, and then runs these steps:
Vector<QueryParam> url_decode(StringView input)
{
    // 1. Let sequences be the result of splitting input on 0x26 (&).
    auto sequences = input.split_view('&');

    // 2. Let output be an initially empty list of name-value tuples where both name and value hold a string.
    Vector<QueryParam> output;

    // 3. For each byte sequence bytes in sequences:
    for (auto bytes : sequences) {
        // 1. If bytes is the empty byte sequence, then continue.
        if (bytes.is_empty())
            continue;

        StringView name;
        StringView value;

        // 2. If bytes contains a 0x3D (=), then let name be the bytes from the start of bytes up to but excluding its first 0x3D (=), and let value be the bytes, if any, after the first 0x3D (=) up to the end of bytes. If 0x3D (=) is the first byte, then name will be the empty byte sequence. If it is the last, then value will be the empty byte sequence.
        if (auto index = bytes.find('='); index.has_value()) {
            name = bytes.substring_view(0, *index);
            value = bytes.substring_view(*index + 1);
        }
        // 3. Otherwise, let name have the value of bytes and let value be the empty byte sequence.
        else {
            name = bytes;
            value = ""sv;
        }

        // 4. Replace any 0x2B (+) in name and value with 0x20 (SP).
        auto space_decoded_name = name.replace("+"sv, " "sv, ReplaceMode::All);
        auto space_decoded_value = value.replace("+"sv, " "sv, ReplaceMode::All);

        // 5. Let nameString and valueString be the result of running UTF-8 decode without BOM on the percent-decoding of name and value, respectively.
        auto name_string = String::from_utf8_with_replacement_character(URL::percent_decode(space_decoded_name), String::WithBOMHandling::No);
        auto value_string = String::from_utf8_with_replacement_character(URL::percent_decode(space_decoded_value), String::WithBOMHandling::No);

        output.empend(move(name_string), move(value_string));
    }

    return output;
}

JS::NonnullGCPtr<URLSearchParams> URLSearchParams::create(JS::Realm& realm, Vector<QueryParam> list)
{
    return realm.heap().allocate<URLSearchParams>(realm, realm, move(list));
}

// https://url.spec.whatwg.org/#urlsearchparams-initialize
JS::NonnullGCPtr<URLSearchParams> URLSearchParams::create(JS::Realm& realm, StringView init)
{
    // NOTE: We skip the other steps since we know it is a string at this point.
    // b. Set query’s list to the result of parsing init.
    return URLSearchParams::create(realm, url_decode(init));
}

// https://url.spec.whatwg.org/#dom-urlsearchparams-urlsearchparams
// https://url.spec.whatwg.org/#urlsearchparams-initialize
WebIDL::ExceptionOr<JS::NonnullGCPtr<URLSearchParams>> URLSearchParams::construct_impl(JS::Realm& realm, Variant<Vector<Vector<String>>, OrderedHashMap<String, String>, String> const& init)
{
    auto& vm = realm.vm();

    // 1. If init is a string and starts with U+003F (?), then remove the first code point from init.
    // NOTE: We do this when we know that it's a string on step 3 of initialization.

    // 2. Initialize this with init.

    // URLSearchParams init from this point forward

    // 1. If init is a sequence, then for each pair in init:
    if (init.has<Vector<Vector<String>>>()) {
        auto const& init_sequence = init.get<Vector<Vector<String>>>();

        Vector<QueryParam> list;
        list.ensure_capacity(init_sequence.size());

        for (auto const& pair : init_sequence) {
            // a. If pair does not contain exactly two items, then throw a TypeError.
            if (pair.size() != 2)
                return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, TRY_OR_THROW_OOM(vm, String::formatted("Expected only 2 items in pair, got {}", pair.size())) };

            // b. Append a new name-value pair whose name is pair’s first item, and value is pair’s second item, to query’s list.
            list.append(QueryParam { .name = pair[0], .value = pair[1] });
        }

        return URLSearchParams::create(realm, move(list));
    }

    // 2. Otherwise, if init is a record, then for each name → value of init, append a new name-value pair whose name is name and value is value, to query’s list.
    if (init.has<OrderedHashMap<String, String>>()) {
        auto const& init_record = init.get<OrderedHashMap<String, String>>();

        Vector<QueryParam> list;
        list.ensure_capacity(init_record.size());

        for (auto const& pair : init_record)
            list.append(QueryParam { .name = pair.key, .value = pair.value });

        return URLSearchParams::create(realm, move(list));
    }

    // 3. Otherwise:
    // a. Assert: init is a string.
    // NOTE: `get` performs `VERIFY(has<T>())`
    auto const& init_string = init.get<String>();

    // See NOTE at the start of this function.
    auto init_string_view = init_string.bytes_as_string_view();
    auto stripped_init = init_string_view.substring_view(init_string_view.starts_with('?'));

    // b. Set query’s list to the result of parsing init.
    return URLSearchParams::create(realm, stripped_init);
}

// https://url.spec.whatwg.org/#dom-urlsearchparams-size
size_t URLSearchParams::size() const
{
    // The size getter steps are to return this’s list’s size.
    return m_list.size();
}

// https://url.spec.whatwg.org/#dom-urlsearchparams-append
void URLSearchParams::append(String const& name, String const& value)
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
    Optional<String> serialized_query = to_string();

    // 3. If serializedQuery is the empty string, then set serializedQuery to null.
    if (serialized_query == String {})
        serialized_query = {};

    // 4. Set query’s URL object’s URL’s query to serializedQuery.
    m_url->set_query({}, move(serialized_query));

    // 5. If serializedQuery is null, then potentially strip trailing spaces from an opaque path with query’s URL object.
    if (!serialized_query.has_value())
        strip_trailing_spaces_from_an_opaque_path(*m_url);
}

// https://url.spec.whatwg.org/#dom-urlsearchparams-delete
void URLSearchParams::delete_(String const& name, Optional<String> const& value)
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

Optional<String> URLSearchParams::get(String const& name)
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
Vector<String> URLSearchParams::get_all(String const& name)
{
    // return the values of all name-value pairs whose name is name, in this’s list, in list order, and the empty sequence otherwise.
    Vector<String> values;
    for (auto& entry : m_list) {
        if (entry.name == name)
            values.append(entry.value);
    }
    return values;
}

// https://url.spec.whatwg.org/#dom-urlsearchparams-has
bool URLSearchParams::has(String const& name, Optional<String> const& value)
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

void URLSearchParams::set(String const& name, String const& value)
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
    // 1. Sort all name-value pairs, if any, by their names. Sorting must be done by comparison of code units. The relative order between name-value pairs with equal names must be preserved.
    insertion_sort(m_list, [](auto& a, auto& b) {
        // FIXME: There should be a way to do this without converting to utf16
        auto a_utf16 = MUST(utf8_to_utf16(a.name));
        auto b_utf16 = MUST(utf8_to_utf16(b.name));

        auto common_length = min(a_utf16.size(), b_utf16.size());

        for (size_t position = 0; position < common_length; ++position) {
            if (a_utf16[position] != b_utf16[position])
                return a_utf16[position] < b_utf16[position];
        }

        return a_utf16.size() < b_utf16.size();
    });

    // 2. Update this.
    update();
}

String URLSearchParams::to_string() const
{
    // return the serialization of this’s list.
    return url_encode(m_list);
}

JS::ThrowCompletionOr<void> URLSearchParams::for_each(ForEachCallback callback)
{
    for (auto i = 0u; i < m_list.size(); ++i) {
        auto& query_param = m_list[i]; // We are explicitly iterating over the indices here as the callback might delete items from the list
        TRY(callback(query_param.name, query_param.value));
    }

    return {};
}

}
