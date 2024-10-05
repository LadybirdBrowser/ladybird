/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2023, networkException <networkexception@serenityos.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/IPv4Address.h>
#include <AK/IPv6Address.h>
#include <LibURL/Parser.h>
#include <LibWeb/Bindings/DOMURLPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/FileAPI/Blob.h>
#include <LibWeb/FileAPI/BlobURLStore.h>

namespace Web::DOMURL {

JS_DEFINE_ALLOCATOR(DOMURL);

JS::NonnullGCPtr<DOMURL> DOMURL::create(JS::Realm& realm, URL::URL url, JS::NonnullGCPtr<URLSearchParams> query)
{
    return realm.heap().allocate<DOMURL>(realm, realm, move(url), move(query));
}

// https://url.spec.whatwg.org/#api-url-parser
static Optional<URL::URL> parse_api_url(String const& url, Optional<String> const& base)
{
    // FIXME: We somewhat awkwardly have two failure states encapsulated in the return type (and convert between them in the steps),
    //        ideally we'd get rid of URL's valid flag

    // 1. Let parsedBase be null.
    Optional<URL::URL> parsed_base;

    // 2. If base is non-null:
    if (base.has_value()) {
        // 1. Set parsedBase to the result of running the basic URL parser on base.
        auto parsed_base_url = URL::Parser::basic_parse(*base);

        // 2. If parsedBase is failure, then return failure.
        if (!parsed_base_url.is_valid())
            return {};

        parsed_base = parsed_base_url;
    }

    // 3. Return the result of running the basic URL parser on url with parsedBase.
    auto parsed = URL::Parser::basic_parse(url, parsed_base);
    return parsed.is_valid() ? parsed : Optional<URL::URL> {};
}

// https://url.spec.whatwg.org/#url-initialize
JS::NonnullGCPtr<DOMURL> DOMURL::initialize_a_url(JS::Realm& realm, URL::URL const& url_record)
{
    // 1. Let query be urlRecord’s query, if that is non-null; otherwise the empty string.
    auto query = url_record.query().value_or(String {});

    // 2. Set url’s URL to urlRecord.
    // 3. Set url’s query object to a new URLSearchParams object.
    auto query_object = URLSearchParams::create(realm, query);

    // 4. Initialize url’s query object with query.
    auto result_url = DOMURL::create(realm, url_record, move(query_object));

    // 5. Set url’s query object’s URL object to url.
    result_url->m_query->m_url = result_url;

    return result_url;
}

// https://url.spec.whatwg.org/#dom-url-parse
JS::GCPtr<DOMURL> DOMURL::parse_for_bindings(JS::VM& vm, String const& url, Optional<String> const& base)
{
    auto& realm = *vm.current_realm();

    // 1. Let parsedURL be the result of running the API URL parser on url with base, if given.
    auto parsed_url = parse_api_url(url, base);

    // 2. If parsedURL is failure, then return null.
    if (!parsed_url.has_value())
        return nullptr;

    // 3. Let url be a new URL object.
    // 4. Initialize url with parsedURL.
    // 5. Return url.
    return initialize_a_url(realm, parsed_url.value());
}

// https://url.spec.whatwg.org/#dom-url-url
WebIDL::ExceptionOr<JS::NonnullGCPtr<DOMURL>> DOMURL::construct_impl(JS::Realm& realm, String const& url, Optional<String> const& base)
{
    // 1. Let parsedURL be the result of running the API URL parser on url with base, if given.
    auto parsed_url = parse_api_url(url, base);

    // 2. If parsedURL is failure, then throw a TypeError.
    if (!parsed_url.has_value())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Invalid URL"sv };

    // 3. Initialize this with parsedURL.
    return initialize_a_url(realm, parsed_url.value());
}

DOMURL::DOMURL(JS::Realm& realm, URL::URL url, JS::NonnullGCPtr<URLSearchParams> query)
    : PlatformObject(realm)
    , m_url(move(url))
    , m_query(move(query))
{
}

DOMURL::~DOMURL() = default;

void DOMURL::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE_WITH_CUSTOM_NAME(DOMURL, URL);
}

void DOMURL::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_query);
}

// https://w3c.github.io/FileAPI/#dfn-createObjectURL
WebIDL::ExceptionOr<String> DOMURL::create_object_url(JS::VM& vm, JS::NonnullGCPtr<FileAPI::Blob> object)
{
    // The createObjectURL(obj) static method must return the result of adding an entry to the blob URL store for obj.
    return TRY_OR_THROW_OOM(vm, FileAPI::add_entry_to_blob_url_store(object));
}

// https://w3c.github.io/FileAPI/#dfn-revokeObjectURL
WebIDL::ExceptionOr<void> DOMURL::revoke_object_url(JS::VM& vm, StringView url)
{
    // 1. Let url record be the result of parsing url.
    auto url_record = parse(url);

    // 2. If url record’s scheme is not "blob", return.
    if (url_record.scheme() != "blob"sv)
        return {};

    // 3. Let origin be the origin of url record.
    auto origin = url_record.origin();

    // 4. Let settings be the current settings object.
    auto& settings = HTML::current_settings_object();

    // 5. If origin is not same origin with settings’s origin, return.
    if (!origin.is_same_origin(settings.origin()))
        return {};

    // 6. Remove an entry from the Blob URL Store for url.
    TRY_OR_THROW_OOM(vm, FileAPI::remove_entry_from_blob_url_store(url));
    return {};
}

// https://url.spec.whatwg.org/#dom-url-canparse
bool DOMURL::can_parse(JS::VM&, String const& url, Optional<String> const& base)
{
    // 1. Let parsedURL be the result of running the API URL parser on url with base, if given.
    auto parsed_url = parse_api_url(url, base);

    // 2. If parsedURL is failure, then return false.
    if (!parsed_url.has_value())
        return false;

    // 3. Return true.
    return true;
}

// https://url.spec.whatwg.org/#dom-url-href
WebIDL::ExceptionOr<String> DOMURL::href() const
{
    auto& vm = realm().vm();

    // The href getter steps and the toJSON() method steps are to return the serialization of this’s URL.
    return TRY_OR_THROW_OOM(vm, String::from_byte_string(m_url.serialize()));
}

// https://url.spec.whatwg.org/#dom-url-tojson
WebIDL::ExceptionOr<String> DOMURL::to_json() const
{
    auto& vm = realm().vm();

    // The href getter steps and the toJSON() method steps are to return the serialization of this’s URL.
    return TRY_OR_THROW_OOM(vm, String::from_byte_string(m_url.serialize()));
}

// https://url.spec.whatwg.org/#ref-for-dom-url-href②
WebIDL::ExceptionOr<void> DOMURL::set_href(String const& href)
{
    // 1. Let parsedURL be the result of running the basic URL parser on the given value.
    URL::URL parsed_url = href;

    // 2. If parsedURL is failure, then throw a TypeError.
    if (!parsed_url.is_valid())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Invalid URL"sv };

    // 3. Set this’s URL to parsedURL.
    m_url = move(parsed_url);

    // 4. Empty this’s query object’s list.
    m_query->m_list.clear();

    // 5. Let query be this’s URL’s query.
    auto query = m_url.query();

    // 6. If query is non-null, then set this’s query object’s list to the result of parsing query.
    if (query.has_value())
        m_query->m_list = url_decode(*query);
    return {};
}

// https://url.spec.whatwg.org/#dom-url-origin
WebIDL::ExceptionOr<String> DOMURL::origin() const
{
    auto& vm = realm().vm();

    // The origin getter steps are to return the serialization of this’s URL’s origin. [HTML]
    return TRY_OR_THROW_OOM(vm, String::from_byte_string(m_url.origin().serialize()));
}

// https://url.spec.whatwg.org/#dom-url-protocol
WebIDL::ExceptionOr<String> DOMURL::protocol() const
{
    auto& vm = realm().vm();

    // The protocol getter steps are to return this’s URL’s scheme, followed by U+003A (:).
    return TRY_OR_THROW_OOM(vm, String::formatted("{}:", m_url.scheme()));
}

// https://url.spec.whatwg.org/#ref-for-dom-url-protocol%E2%91%A0
WebIDL::ExceptionOr<void> DOMURL::set_protocol(String const& protocol)
{
    auto& vm = realm().vm();

    // The protocol setter steps are to basic URL parse the given value, followed by U+003A (:), with this’s URL as
    // url and scheme start state as state override.
    (void)URL::Parser::basic_parse(TRY_OR_THROW_OOM(vm, String::formatted("{}:", protocol)), {}, &m_url, URL::Parser::State::SchemeStart);
    return {};
}

// https://url.spec.whatwg.org/#dom-url-username
String const& DOMURL::username() const
{
    // The username getter steps are to return this’s URL’s username.
    return m_url.username();
}

// https://url.spec.whatwg.org/#ref-for-dom-url-username%E2%91%A0
void DOMURL::set_username(String const& username)
{
    // 1. If this’s URL cannot have a username/password/port, then return.
    if (m_url.cannot_have_a_username_or_password_or_port())
        return;

    // 2. Set the username given this’s URL and the given value.
    m_url.set_username(username);
}

// https://url.spec.whatwg.org/#dom-url-password
String const& DOMURL::password() const
{
    // The password getter steps are to return this’s URL’s password.
    return m_url.password();
}

// https://url.spec.whatwg.org/#ref-for-dom-url-password%E2%91%A0
void DOMURL::set_password(String const& password)
{
    // 1. If this’s URL cannot have a username/password/port, then return.
    if (m_url.cannot_have_a_username_or_password_or_port())
        return;

    // 2. Set the password given this’s URL and the given value.
    m_url.set_password(password);
}

// https://url.spec.whatwg.org/#dom-url-host
WebIDL::ExceptionOr<String> DOMURL::host() const
{
    auto& vm = realm().vm();

    // 1. Let url be this’s URL.
    auto& url = m_url;

    // 2. If url’s host is null, then return the empty string.
    if (url.host().has<Empty>())
        return String {};

    // 3. If url’s port is null, return url’s host, serialized.
    if (!url.port().has_value())
        return TRY_OR_THROW_OOM(vm, url.serialized_host());

    // 4. Return url’s host, serialized, followed by U+003A (:) and url’s port, serialized.
    return TRY_OR_THROW_OOM(vm, String::formatted("{}:{}", TRY_OR_THROW_OOM(vm, url.serialized_host()), *url.port()));
}

// https://url.spec.whatwg.org/#dom-url-hostref-for-dom-url-host%E2%91%A0
void DOMURL::set_host(String const& host)
{
    // 1. If this’s URL’s cannot-be-a-base-URL is true, then return.
    if (m_url.cannot_be_a_base_url())
        return;

    // 2. Basic URL parse the given value with this’s URL as url and host state as state override.
    (void)URL::Parser::basic_parse(host, {}, &m_url, URL::Parser::State::Host);
}

// https://url.spec.whatwg.org/#dom-url-hostname
WebIDL::ExceptionOr<String> DOMURL::hostname() const
{
    auto& vm = realm().vm();

    // 1. If this’s URL’s host is null, then return the empty string.
    if (m_url.host().has<Empty>())
        return String {};

    // 2. Return this’s URL’s host, serialized.
    return TRY_OR_THROW_OOM(vm, m_url.serialized_host());
}

// https://url.spec.whatwg.org/#ref-for-dom-url-hostname①
void DOMURL::set_hostname(String const& hostname)
{
    // 1. If this’s URL’s cannot-be-a-base-URL is true, then return.
    if (m_url.cannot_be_a_base_url())
        return;

    // 2. Basic URL parse the given value with this’s URL as url and hostname state as state override.
    (void)URL::Parser::basic_parse(hostname, {}, &m_url, URL::Parser::State::Hostname);
}

// https://url.spec.whatwg.org/#dom-url-port
WebIDL::ExceptionOr<String> DOMURL::port() const
{
    auto& vm = realm().vm();

    // 1. If this’s URL’s port is null, then return the empty string.
    if (!m_url.port().has_value())
        return String {};

    // 2. Return this’s URL’s port, serialized.
    return TRY_OR_THROW_OOM(vm, String::formatted("{}", *m_url.port()));
}

// https://url.spec.whatwg.org/#ref-for-dom-url-port%E2%91%A0
void DOMURL::set_port(String const& port)
{
    // 1. If this’s URL cannot have a username/password/port, then return.
    if (m_url.cannot_have_a_username_or_password_or_port())
        return;

    // 2. If the given value is the empty string, then set this’s URL’s port to null.
    if (port.is_empty()) {
        m_url.set_port({});
    }
    // 3. Otherwise, basic URL parse the given value with this’s URL as url and port state as state override.
    else {
        (void)URL::Parser::basic_parse(port, {}, &m_url, URL::Parser::State::Port);
    }
}

// https://url.spec.whatwg.org/#dom-url-pathname
String DOMURL::pathname() const
{
    // The pathname getter steps are to return the result of URL path serializing this’s URL.
    return m_url.serialize_path();
}

// https://url.spec.whatwg.org/#ref-for-dom-url-pathname%E2%91%A0
void DOMURL::set_pathname(String const& pathname)
{
    // FIXME: These steps no longer match the speci.
    // 1. If this’s URL’s cannot-be-a-base-URL is true, then return.
    if (m_url.cannot_be_a_base_url())
        return;

    // 2. Empty this’s URL’s path.
    m_url.set_paths({});

    // 3. Basic URL parse the given value with this’s URL as url and path start state as state override.
    (void)URL::Parser::basic_parse(pathname, {}, &m_url, URL::Parser::State::PathStart);
}

// https://url.spec.whatwg.org/#dom-url-search
WebIDL::ExceptionOr<String> DOMURL::search() const
{
    auto& vm = realm().vm();

    // 1. If this’s URL’s query is either null or the empty string, then return the empty string.
    if (!m_url.query().has_value() || m_url.query()->is_empty())
        return String {};

    // 2. Return U+003F (?), followed by this’s URL’s query.
    return TRY_OR_THROW_OOM(vm, String::formatted("?{}", *m_url.query()));
}

// https://url.spec.whatwg.org/#ref-for-dom-url-search%E2%91%A0
void DOMURL::set_search(String const& search)
{
    // 1. Let url be this’s URL.
    auto& url = m_url;

    // 2. If the given value is the empty string:
    if (search.is_empty()) {
        // 1. Set url’s query to null.
        url.set_query({});

        // 2. Empty this’s query object’s list.
        m_query->m_list.clear();

        // 3. Potentially strip trailing spaces from an opaque path with this.
        strip_trailing_spaces_from_an_opaque_path(*this);

        // 4. Return.
        return;
    }

    // 3. Let input be the given value with a single leading U+003F (?) removed, if any.
    auto search_as_string_view = search.bytes_as_string_view();
    auto input = search_as_string_view.substring_view(search_as_string_view.starts_with('?'));

    // 4. Set url’s query to the empty string.
    url.set_query(String {});

    // 5. Basic URL parse input with url as url and query state as state override.
    (void)URL::Parser::basic_parse(input, {}, &url, URL::Parser::State::Query);

    // 6. Set this’s query object’s list to the result of parsing input.
    m_query->m_list = url_decode(input);
}

// https://url.spec.whatwg.org/#dom-url-searchparams
JS::NonnullGCPtr<URLSearchParams const> DOMURL::search_params() const
{
    // The searchParams getter steps are to return this’s query object.
    return m_query;
}

// https://url.spec.whatwg.org/#dom-url-hash
WebIDL::ExceptionOr<String> DOMURL::hash() const
{
    auto& vm = realm().vm();

    // 1. If this’s URL’s fragment is either null or the empty string, then return the empty string.
    if (!m_url.fragment().has_value() || m_url.fragment()->is_empty())
        return String {};

    // 2. Return U+0023 (#), followed by this’s URL’s fragment.
    return TRY_OR_THROW_OOM(vm, String::formatted("#{}", m_url.fragment()));
}

// https://url.spec.whatwg.org/#ref-for-dom-url-hash%E2%91%A0
void DOMURL::set_hash(String const& hash)
{
    // 1. If the given value is the empty string:
    if (hash.is_empty()) {
        // 1. Set this’s URL’s fragment to null.
        m_url.set_fragment({});

        // 2. Potentially strip trailing spaces from an opaque path with this.
        strip_trailing_spaces_from_an_opaque_path(*this);

        // 3. Return.
        return;
    }

    // 2. Let input be the given value with a single leading U+0023 (#) removed, if any.
    auto hash_as_string_view = hash.bytes_as_string_view();
    auto input = hash_as_string_view.substring_view(hash_as_string_view.starts_with('#'));

    // 3. Set this’s URL’s fragment to the empty string.
    m_url.set_fragment(String {});

    // 4. Basic URL parse input with this’s URL as url and fragment state as state override.
    (void)URL::Parser::basic_parse(input, {}, &m_url, URL::Parser::State::Fragment);
}

// https://url.spec.whatwg.org/#concept-domain
bool host_is_domain(URL::Host const& host)
{
    // A domain is a non-empty ASCII string that identifies a realm within a network.
    return host.has<String>() && host.get<String>() != String {};
}

// https://url.spec.whatwg.org/#potentially-strip-trailing-spaces-from-an-opaque-path
void strip_trailing_spaces_from_an_opaque_path(DOMURL& url)
{
    // 1. If url’s URL does not have an opaque path, then return.
    // FIXME: Reimplement this step once we modernize the URL implementation to meet the spec.
    if (!url.cannot_be_a_base_url())
        return;

    // 2. If url’s URL’s fragment is non-null, then return.
    if (url.fragment().has_value())
        return;

    // 3. If url’s URL’s query is non-null, then return.
    if (url.query().has_value())
        return;

    // 4. Remove all trailing U+0020 SPACE code points from url’s URL’s path.
    // NOTE: At index 0 since the first step tells us that the URL only has one path segment.
    auto opaque_path = url.path_segment_at_index(0);
    auto trimmed_path = opaque_path.trim(" "sv, TrimMode::Right);
    url.set_paths({ trimmed_path });
}

// https://url.spec.whatwg.org/#concept-url-parser
URL::URL parse(StringView input, Optional<URL::URL> const& base_url, Optional<StringView> encoding)
{
    // FIXME: We should probably have an extended version of URL::URL for LibWeb instead of standalone functions like this.

    // 1. Let url be the result of running the basic URL parser on input with base and encoding.
    auto url = URL::Parser::basic_parse(input, base_url, {}, {}, encoding);

    // 2. If url is failure, return failure.
    if (!url.is_valid())
        return {};

    // 3. If url’s scheme is not "blob", return url.
    if (url.scheme() != "blob")
        return url;

    // 4. Set url’s blob URL entry to the result of resolving the blob URL url, if that did not return failure, and null otherwise.
    auto blob_url_entry = FileAPI::resolve_a_blob_url(url);
    if (blob_url_entry.has_value()) {
        url.set_blob_url_entry(URL::BlobURLEntry {
            .type = blob_url_entry->object->type(),
            .byte_buffer = MUST(ByteBuffer::copy(blob_url_entry->object->raw_bytes())),
            .environment_origin = blob_url_entry->environment->origin(),
        });
    }

    // 5. Return url
    return url;
}

}
