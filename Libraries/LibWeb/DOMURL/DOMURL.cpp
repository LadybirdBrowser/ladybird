/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2023, networkException <networkexception@serenityos.org>
 * Copyright (c) 2024-2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibURL/Parser.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/FileAPI/Blob.h>
#include <LibWeb/FileAPI/BlobURLStore.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/Infra/SerializedURL.h>
#include <LibWeb/MediaSourceExtensions/MediaSource.h>
#include <LibWeb/Page/Page.h>

namespace Web::DOMURL {

// Called by Bindings/Rust/src/domurl/url.rs.
extern "C" {
FlatPtr web_url_create_object_url_for_blob(FileAPI::Blob* blob);
FlatPtr web_url_create_object_url_for_media_source(JSObject* object);
void web_url_revoke_object_url(FlatPtr raw_url);
}

// https://w3c.github.io/FileAPI/#dfn-createObjectURL
static FlatPtr create_object_url(FileAPI::BlobURLEntry::Object obj)
{
    // The createObjectURL(obj) static method must return the result of adding an entry to the blob URL store for obj.
    return MUST(FileAPI::add_entry_to_blob_url_store(obj)).into_raw();
}

extern "C" FlatPtr web_url_create_object_url_for_blob(FileAPI::Blob* blob)
{
    return create_object_url(GC::Ref { *blob });
}

extern "C" FlatPtr web_url_create_object_url_for_media_source(JSObject* object)
{
    auto* wrappable = Bindings::wrappable_impl_from(JS::HostABI::object_from_abi(object));
    return create_object_url(GC::Ref { as<MediaSourceExtensions::MediaSource>(*wrappable) });
}

// https://w3c.github.io/FileAPI/#dfn-revokeObjectURL
extern "C" void web_url_revoke_object_url(FlatPtr raw_url)
{
    auto url = Utf16String::from_raw(raw_url);

    // 1. Let url record be the result of parsing url.
    auto url_record = parse(url.utf16_view());

    // Spec Bug: https://github.com/w3c/FileAPI/issues/207, missing check for URL failure parsing.
    if (!url_record.has_value())
        return;

    // 2. If url record’s scheme is not "blob", return.
    if (url_record->scheme() != "blob"sv)
        return;

    // NB: The browser process runs steps 3 to 7 over every process's entries. The steps below run them again here,
    //     for the entry this process created.
    auto& settings = HTML::current_settings_object();
    Bindings::principal_host_defined_page(settings.realm()).client().page_did_remove_blob_url_entries(settings, { utf16_string_from_url_ascii(url_record->serialize()) });

    // 3. Let entry be urlRecord’s blob URL entry.
    auto const& entry = url_record->blob_url_entry();

    // 4. If entry is null, return.
    if (!entry.has_value())
        return;

    // 5. Let isAuthorized be the result of checking for same-partition blob URL usage with entry and the current settings object.
    bool is_authorized = FileAPI::check_for_same_partition_blob_url_usage(entry->environment.origin, HTML::current_settings_object());

    // 6. If isAuthorized is false, then return.
    if (!is_authorized)
        return;

    // 7. Remove an entry from the Blob URL Store for url.
    // FIXME: Spec bug: https://github.com/w3c/FileAPI/issues/207, urlRecord should instead be passed through.
    FileAPI::remove_entry_from_blob_url_store(*url_record);
}

// https://url.spec.whatwg.org/#concept-url-parser
static Optional<URL::URL> finish_parsing(Optional<URL::URL> url)
{
    // FIXME: We should probably have an extended version of URL::URL for LibWeb instead of standalone functions like this.

    // 2. If url is failure, return failure.
    if (!url.has_value())
        return {};

    // 3. If url’s scheme is not "blob", return url.
    if (url->scheme() != "blob")
        return url.release_value();

    // 4. Set url’s blob URL entry to the result of resolving the blob URL url, if that did not return failure, and null otherwise.
    url->set_blob_url_entry(FileAPI::resolve_a_blob_url(*url));

    // 5. Return url
    return url.release_value();
}

Optional<URL::URL> parse_from_byte_string(StringView input, Optional<URL::URL const&> base_url, Optional<StringView> encoding)
{
    // 1. Let url be the result of running the basic URL parser on input with base and encoding.
    return finish_parsing(URL::Parser::basic_parse(input, base_url, encoding));
}

Optional<URL::URL> parse(Utf16View input, Optional<URL::URL const&> base_url, Optional<Utf16View> encoding)
{
    Optional<String> encoding_utf8;
    Optional<StringView> encoding_view;
    if (encoding.has_value()) {
        encoding_utf8 = MUST(encoding->to_utf8());
        encoding_view = encoding_utf8->bytes_as_string_view();
    }
    // 1. Let url be the result of running the basic URL parser on input with base and encoding.
    return finish_parsing(URL::Parser::basic_parse(input, base_url, encoding_view));
}

}
