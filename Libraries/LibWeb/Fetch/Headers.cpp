/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/HeadersPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Fetch/Headers.h>

namespace Web::Fetch {

JS_DEFINE_ALLOCATOR(Headers);

// https://fetch.spec.whatwg.org/#dom-headers
WebIDL::ExceptionOr<JS::NonnullGCPtr<Headers>> Headers::construct_impl(JS::Realm& realm, Optional<HeadersInit> const& init)
{
    auto& vm = realm.vm();

    // The new Headers(init) constructor steps are:
    auto headers = realm.heap().allocate<Headers>(realm, realm, Infrastructure::HeaderList::create(vm));

    // 1. Set this’s guard to "none".
    headers->m_guard = Guard::None;

    // 2. If init is given, then fill this with init.
    if (init.has_value())
        TRY(headers->fill(*init));

    return headers;
}

Headers::Headers(JS::Realm& realm, JS::NonnullGCPtr<Infrastructure::HeaderList> header_list)
    : PlatformObject(realm)
    , m_header_list(header_list)
{
}

Headers::~Headers() = default;

void Headers::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Headers);
}

void Headers::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_header_list);
}

// https://fetch.spec.whatwg.org/#dom-headers-append
WebIDL::ExceptionOr<void> Headers::append(String const& name_string, String const& value_string)
{
    // The append(name, value) method steps are to append (name, value) to this.
    auto header = Infrastructure::Header {
        .name = MUST(ByteBuffer::copy(name_string.bytes())),
        .value = MUST(ByteBuffer::copy(value_string.bytes())),
    };
    TRY(append(move(header)));
    return {};
}

// https://fetch.spec.whatwg.org/#dom-headers-delete
WebIDL::ExceptionOr<void> Headers::delete_(String const& name_string)
{
    // The delete(name) method steps are:
    auto name = name_string.bytes();

    // 1. If validating (name, ``) for headers returns false, then return.
    // NOTE: Passing a dummy header value ought not to have any negative repercussions.
    auto header = Infrastructure::Header::from_string_pair(name, ""sv);
    if (!TRY(validate(header)))
        return {};

    // 2. If this’s guard is "request-no-cors", name is not a no-CORS-safelisted request-header name, and name is not a privileged no-CORS request-header name, then return.
    if (m_guard == Guard::RequestNoCORS && !Infrastructure::is_no_cors_safelisted_request_header_name(name) && !Infrastructure::is_privileged_no_cors_request_header_name(name))
        return {};

    // 3. If this’s header list does not contain name, then return.
    if (!m_header_list->contains(name))
        return {};

    // 4. Delete name from this’s header list.
    m_header_list->delete_(name);

    // 5. If this’s guard is "request-no-cors", then remove privileged no-CORS request-headers from this.
    if (m_guard == Guard::RequestNoCORS)
        remove_privileged_no_cors_request_headers();

    return {};
}

// https://fetch.spec.whatwg.org/#dom-headers-get
WebIDL::ExceptionOr<Optional<String>> Headers::get(String const& name_string)
{
    // The get(name) method steps are:
    auto name = name_string.bytes();

    // 1. If name is not a header name, then throw a TypeError.
    if (!Infrastructure::is_header_name(name))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Invalid header name"sv };

    // 2. Return the result of getting name from this’s header list.
    auto byte_buffer = m_header_list->get(name);
    return byte_buffer.has_value() ? MUST(String::from_utf8(*byte_buffer)) : Optional<String> {};
}

// https://fetch.spec.whatwg.org/#dom-headers-getsetcookie
Vector<String> Headers::get_set_cookie()
{
    // The getSetCookie() method steps are:
    auto values = Vector<String> {};

    // 1. If this’s header list does not contain `Set-Cookie`, then return « ».
    if (!m_header_list->contains("Set-Cookie"sv.bytes()))
        return values;

    // 2. Return the values of all headers in this’s header list whose name is a byte-case-insensitive match for
    //    `Set-Cookie`, in order.
    for (auto const& header : *m_header_list) {
        if (StringView { header.name }.equals_ignoring_ascii_case("Set-Cookie"sv))
            values.append(MUST(String::from_utf8(header.value)));
    }
    return values;
}

// https://fetch.spec.whatwg.org/#dom-headers-has
WebIDL::ExceptionOr<bool> Headers::has(String const& name_string)
{
    // The has(name) method steps are:
    auto name = name_string.bytes();

    // 1. If name is not a header name, then throw a TypeError.
    if (!Infrastructure::is_header_name(name))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Invalid header name"sv };

    // 2. Return true if this’s header list contains name; otherwise false.
    return m_header_list->contains(name);
}

// https://fetch.spec.whatwg.org/#dom-headers-set
WebIDL::ExceptionOr<void> Headers::set(String const& name_string, String const& value_string)
{
    // The set(name, value) method steps are:
    auto name = name_string.bytes();
    auto value = value_string.bytes();

    // 1. Normalize value.
    auto normalized_value = Infrastructure::normalize_header_value(value);

    auto header = Infrastructure::Header {
        .name = MUST(ByteBuffer::copy(name)),
        .value = move(normalized_value),
    };

    // 2. If validating (name, value) for headers returns false, then return.
    if (!TRY(validate(header)))
        return {};

    // 3. If this’s guard is "request-no-cors" and (name, value) is not a no-CORS-safelisted request-header, then return.
    if (m_guard == Guard::RequestNoCORS && !Infrastructure::is_no_cors_safelisted_request_header(header))
        return {};

    // 4. Set (name, value) in this’s header list.
    m_header_list->set(move(header));

    // 5. If this’s guard is "request-no-cors", then remove privileged no-CORS request-headers from this.
    if (m_guard == Guard::RequestNoCORS)
        remove_privileged_no_cors_request_headers();

    return {};
}

// https://webidl.spec.whatwg.org/#es-iterable, Step 4
JS::ThrowCompletionOr<void> Headers::for_each(ForEachCallback callback)
{
    // The value pairs to iterate over are the return value of running sort and combine with this’s header list.
    auto value_pairs_to_iterate_over = [&]() {
        return m_header_list->sort_and_combine();
    };

    // 1-5. Are done in the generated wrapper code.

    // 6. Let pairs be idlObject’s list of value pairs to iterate over.
    auto pairs = value_pairs_to_iterate_over();

    // 7. Let i be 0.
    size_t i = 0;

    // 8. While i < pairs’s size:
    while (i < pairs.size()) {
        // 1. Let pair be pairs[i].
        auto const& pair = pairs[i];

        // 2. Invoke idlCallback with « pair’s value, pair’s key, idlObject » and with thisArg as the callback this value.
        TRY(callback(MUST(String::from_utf8(pair.name)), MUST(String::from_utf8(pair.value))));

        // 3. Set pairs to idlObject’s current list of value pairs to iterate over. (It might have changed.)
        pairs = value_pairs_to_iterate_over();

        // 4. Set i to i + 1.
        ++i;
    }

    return {};
}

// https://fetch.spec.whatwg.org/#headers-validate
WebIDL::ExceptionOr<bool> Headers::validate(Infrastructure::Header const& header) const
{
    // To validate a header (name, value) for a Headers object headers:
    auto const& [name, value] = header;

    // 1. If name is not a header name or value is not a header value, then throw a TypeError.
    if (!Infrastructure::is_header_name(name))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Invalid header name"sv };
    if (!Infrastructure::is_header_value(value))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Invalid header value"sv };

    // 2. If headers’s guard is "immutable", then throw a TypeError.
    if (m_guard == Guard::Immutable)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Headers object is immutable"sv };

    // 3. If headers’s guard is "request" and (name, value) is a forbidden request-header, then return false.
    if (m_guard == Guard::Request && Infrastructure::is_forbidden_request_header(header))
        return false;

    // 4. If headers’s guard is "response" and name is a forbidden response-header name, then return false.
    if (m_guard == Guard::Response && Infrastructure::is_forbidden_response_header_name(name))
        return false;

    // 5. Return true.
    return true;
}

// https://fetch.spec.whatwg.org/#concept-headers-append
WebIDL::ExceptionOr<void> Headers::append(Infrastructure::Header header)
{
    // To append a header (name, value) to a Headers object headers, run these steps:
    auto& [name, value] = header;

    // 1. Normalize value.
    value = Infrastructure::normalize_header_value(value);

    // 2. If validating (name, value) for headers returns false, then return.
    if (!TRY(validate(header)))
        return {};

    // 3. If headers’s guard is "request-no-cors":
    if (m_guard == Guard::RequestNoCORS) {
        // 1. Let temporaryValue be the result of getting name from headers’s header list.
        auto temporary_value = m_header_list->get(name);

        // 2. If temporaryValue is null, then set temporaryValue to value.
        if (!temporary_value.has_value()) {
            temporary_value = MUST(ByteBuffer::copy(value));
        }
        // 3. Otherwise, set temporaryValue to temporaryValue, followed by 0x2C 0x20, followed by value.
        else {
            temporary_value->append(0x2c);
            temporary_value->append(0x20);
            temporary_value->append(value);
        }

        auto temporary_header = Infrastructure::Header {
            .name = MUST(ByteBuffer::copy(name)),
            .value = temporary_value.release_value(),
        };

        // 4. If (name, temporaryValue) is not a no-CORS-safelisted request-header, then return.
        if (!Infrastructure::is_no_cors_safelisted_request_header(temporary_header))
            return {};
    }

    // 4. Append (name, value) to headers’s header list.
    m_header_list->append(move(header));

    // 5. If headers’s guard is "request-no-cors", then remove privileged no-CORS request-headers from headers.
    if (m_guard == Guard::RequestNoCORS)
        remove_privileged_no_cors_request_headers();

    return {};
}

// https://fetch.spec.whatwg.org/#concept-headers-fill
WebIDL::ExceptionOr<void> Headers::fill(HeadersInit const& object)
{
    // To fill a Headers object headers with a given object object, run these steps:
    return object.visit(
        // 1. If object is a sequence, then for each header of object:
        [&](Vector<Vector<String>> const& object) -> WebIDL::ExceptionOr<void> {
            for (auto const& entry : object) {
                // 1. If header's size is not 2, then throw a TypeError.
                if (entry.size() != 2)
                    return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Array must contain header key/value pair"sv };

                // 2. Append (header[0], header[1]) to headers.
                auto header = Infrastructure::Header::from_string_pair(entry[0], entry[1]);
                TRY(append(move(header)));
            }
            return {};
        },
        // 2. Otherwise, object is a record, then for each key → value of object, append (key, value) to headers.
        [&](OrderedHashMap<String, String> const& object) -> WebIDL::ExceptionOr<void> {
            for (auto const& entry : object) {
                auto header = Infrastructure::Header::from_string_pair(entry.key, entry.value);
                TRY(append(move(header)));
            }
            return {};
        });
}

// https://fetch.spec.whatwg.org/#concept-headers-remove-privileged-no-cors-request-headers
void Headers::remove_privileged_no_cors_request_headers()
{
    // To remove privileged no-CORS request-headers from a Headers object (headers), run these steps:

    static constexpr Array privileged_no_cors_request_header_names = {
        "Range"sv,
    };

    // 1. For each headerName of privileged no-CORS request-header names:
    for (auto const& header_name : privileged_no_cors_request_header_names) {
        // 1. Delete headerName from headers’s header list.
        m_header_list->delete_(header_name.bytes());
    }
}

}
