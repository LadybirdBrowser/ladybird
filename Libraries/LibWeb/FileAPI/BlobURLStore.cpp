/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024-2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <AK/Utf16StringBuilder.h>
#include <LibGC/ConservativeHashMap.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/Crypto/Crypto.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/FileAPI/Blob.h>
#include <LibWeb/FileAPI/BlobURLStore.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/Infra/SerializedURL.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/StorageAPI/StorageKey.h>

namespace Web::FileAPI {

// https://w3c.github.io/FileAPI/#BlobURLStore
// NB: Only the entries this process created. The browser process keeps every process's entries, and is asked for
//     them by blob_url_entry_in_the_user_agent_store().
static GC::ConservativeHashMap<Utf16String, BlobURLEntry>& blob_url_store()
{
    static NeverDestroyed<GC::ConservativeHashMap<Utf16String, BlobURLEntry>> store;
    return *store;
}

// The entry as the user agent's blob URL store has it: a Blob's bytes in shared memory, and the environment's origin.
static ErrorOr<SerializedBlobURLEntry> serialize_blob_url_entry(BlobURLEntry const& entry)
{
    auto object = TRY(entry.object.visit(
        [](GC::Ref<Blob> const& blob) -> ErrorOr<SerializedBlobURLEntry::Object> {
            return SerializedBlobURLEntry::Blob { .type = blob->type().to_utf8(), .data = TRY(blob->shared_bytes()) };
        },
        [](GC::Ref<MediaSourceExtensions::MediaSource> const&) -> ErrorOr<SerializedBlobURLEntry::Object> {
            return SerializedBlobURLEntry::MediaSource {};
        }));

    return SerializedBlobURLEntry { .object = move(object), .origin = entry.environment->origin() };
}

// https://w3c.github.io/FileAPI/#unicodeBlobURL
Utf16String generate_new_blob_url()
{
    // 1. Let result be the empty string.
    Utf16StringBuilder result;

    // 2. Append the string "blob:" to result.
    result.append_ascii("blob:"sv);

    // 3. Let settings be the current settings object
    auto& settings = HTML::current_settings_object();

    // 4. Let origin be settings’s origin.
    auto origin = settings.origin();

    // 5. Let serialized be the ASCII serialization of origin.
    auto serialized = origin.serialize();

    // 6. If serialized is "null", set it to an implementation-defined value.
    if (serialized == "null"sv)
        serialized = "ladybird"_string;

    // 7. Append serialized to result.
    result.append_ascii(serialized.bytes_as_string_view());

    // 8. Append U+0024 SOLIDUS (/) to result.
    result.append_ascii('/');

    // 9. Generate a UUID [RFC4122] as a string and append it to result.
    auto uuid = Crypto::generate_random_uuid();
    result.append_ascii(uuid.bytes_as_string_view());

    // 10. Return result.
    return result.to_string();
}

// https://w3c.github.io/FileAPI/#add-an-entry
ErrorOr<Utf16String> add_entry_to_blob_url_store(BlobURLEntry::Object object)
{
    // 1. Let store be the user agent’s blob URL store.
    auto& store = blob_url_store();

    // 2. Let url be the result of generating a new blob URL.
    auto url = generate_new_blob_url();

    // 3. Let entry be a new blob URL entry consisting of object and the current settings object.
    auto& settings = HTML::current_settings_object();
    BlobURLEntry entry { object, settings };

    // 4. Set store[url] to entry.
    entry.token = Bindings::principal_host_defined_page(settings.realm()).client().page_did_add_blob_url_entry(url, TRY(serialize_blob_url_entry(entry)));
    store.set(url, move(entry));

    // 5. Return url.
    return url;
}

// https://www.w3.org/TR/FileAPI/#check-for-same-partition-blob-url-usage
bool check_for_same_partition_blob_url_usage(URL::Origin const& blob_url_entry_origin, GC::Ref<HTML::Environment> environment)
{
    // 2. Let environmentStorageKey be the result of obtaining a storage key for non-storage purposes with environment.
    auto environment_storage_key = StorageAPI::obtain_a_storage_key_for_non_storage_purposes(environment);

    return check_for_same_partition_blob_url_usage(blob_url_entry_origin, environment_storage_key.origin);
}

// https://www.w3.org/TR/FileAPI/#check-for-same-partition-blob-url-usage
// NB: A storage key for non-storage purposes is just an origin, which is all the browser process has of an environment.
bool check_for_same_partition_blob_url_usage(URL::Origin const& blob_url_entry_origin, URL::Origin const& environment_origin)
{
    // 1. Let blobStorageKey be the result of obtaining a storage key for non-storage purposes with blobUrlEntry’s environment.
    auto blob_storage_key = StorageAPI::obtain_a_storage_key_for_non_storage_purposes(blob_url_entry_origin);

    // 2. Let environmentStorageKey be the result of obtaining a storage key for non-storage purposes with environment.
    auto environment_storage_key = StorageAPI::obtain_a_storage_key_for_non_storage_purposes(environment_origin);

    // 3. If blobStorageKey is not equal to environmentStorageKey, then return false.
    if (blob_storage_key != environment_storage_key)
        return false;

    // 4. Return true.
    return true;
}

// https://www.w3.org/TR/FileAPI/#blob-url-obtain-object
Optional<SerializedBlobURLEntry::Object> obtain_a_blob_object(SerializedBlobURLEntry const& blob_url_entry, Variant<GC::Ref<HTML::Environment>, TopLevelNavigation, TopLevelSelfFetch> environment)
{
    // 1. Let isAuthorized be true.
    bool is_authorized = true;

    // 2. If environment is an environment settings object, then set isAuthorized to the result of checking for same-partition blob URL usage with blobUrlEntry and environment.
    if (environment.has<GC::Ref<HTML::Environment>>())
        is_authorized = check_for_same_partition_blob_url_usage(blob_url_entry.origin, environment.get<GC::Ref<HTML::Environment>>());

    // 3. If isAuthorized is false, then return failure.
    if (!is_authorized)
        return {};

    // 4. Return blobUrlEntry’s object.
    return blob_url_entry.object;
}

// https://w3c.github.io/FileAPI/#removeTheEntry
void remove_entry_from_blob_url_store(URL::URL const& url)
{
    // 1. Let store be the user agent’s blob URL store;
    auto& store = blob_url_store();

    // 2. Let url string be the result of serializing url.
    auto url_string = utf16_string_from_url_ascii(url.serialize());

    // 3. Remove store[url string].
    store.remove(url_string);
}

// https://w3c.github.io/FileAPI/#lifeTime
void run_unloading_cleanup_steps(GC::Ref<DOM::Document> document)
{
    // 1.  Let environment be the Document's relevant settings object.
    auto& environment = document->relevant_settings_object();

    // 2.  Let store be the user agent’s blob URL store;
    auto& store = FileAPI::blob_url_store();

    // 3. Remove from store any entries for which the value's environment is equal to environment.
    Vector<Utf16String> urls;
    store.remove_all_matching([&](auto const& url, auto const& entry) {
        if (entry.environment.ptr() != &environment)
            return false;
        urls.append(url);
        return true;
    });
    // NB: Remove them from the browser process's store too, as revokeObjectURL would.
    if (!urls.is_empty())
        document->page().client().page_did_remove_blob_url_entries(urls, environment.origin());
}

// https://w3c.github.io/FileAPI/#blob-url-resolve
Optional<URL::BlobURLEntry> resolve_a_blob_url(URL::URL const& url)
{
    // 1. Assert: url’s scheme is "blob".
    VERIFY(url.scheme() == "blob"sv);

    // 2. Let store be the user agent’s blob URL store.
    auto& store = blob_url_store();

    // 3. Let url string be the result of serializing url with the exclude fragment flag set.
    auto url_string = utf16_string_from_url_ascii(url.serialize(URL::ExcludeFragment::Yes));

    // 4. If store[url string] exists, return store[url string]; otherwise return failure.
    auto entry = store.get(url_string);
    if (!entry.has_value())
        return {};

    auto object = entry->object.visit(
        [&](GC::Ref<Blob> const& blob) -> ErrorOr<URL::BlobURLEntry::Object> {
            return URL::BlobURLEntry::Blob { entry->token, BlobURLObject::create(blob->type().to_utf8(), TRY(blob->shared_bytes())) };
        },
        [](GC::Ref<MediaSourceExtensions::MediaSource> const&) -> ErrorOr<URL::BlobURLEntry::Object> {
            return URL::BlobURLEntry::MediaSource {};
        });
    if (object.is_error())
        return {};
    return URL::BlobURLEntry { .object = object.release_value(), .environment { .origin = entry->environment->origin() } };
}

// https://url.spec.whatwg.org/#concept-url-blob-entry
// The blob URL entry of url, from the URL record itself if the parser resolved one, and from the browser process
// otherwise.
Optional<SerializedBlobURLEntry> blob_url_entry_in_the_user_agent_store(Page& page, URL::URL const& url)
{
    if (url.scheme() != "blob"sv)
        return {};

    Optional<URL::BlobURLEntry::Token> token;

    if (auto const& entry = url.blob_url_entry(); entry.has_value()) {
        auto object = entry->object.visit(
            [&](URL::BlobURLEntry::Blob const& blob) -> Optional<SerializedBlobURLEntry::Object> {
                token = blob.token;
                if (!blob.object)
                    return {};
                auto const& blob_object = as<BlobURLObject>(*blob.object);
                return SerializedBlobURLEntry::Blob { .token = blob.token, .type = blob_object.type(), .data = blob_object.data() };
            },
            [](URL::BlobURLEntry::MediaSource const&) -> Optional<SerializedBlobURLEntry::Object> {
                return SerializedBlobURLEntry::MediaSource {};
            });
        if (object.has_value())
            return SerializedBlobURLEntry { .object = object.release_value(), .origin = entry->environment.origin };
    }

    // https://w3c.github.io/FileAPI/#blob-url-resolve
    // 3. Let url string be the result of serializing url with the exclude fragment flag set.
    auto url_string = utf16_string_from_url_ascii(url.serialize(URL::ExcludeFragment::Yes));

    // 4. If store[url string] exists, return store[url string]; otherwise return failure.
    return page.client().page_did_request_blob_url_entry(url_string, token);
}

// The entry as this process's own store has it, for callers that need the object itself and not a copy of its bytes.
Optional<BlobURLEntry const&> local_blob_url_entry(URL::URL const& url)
{
    auto url_string = utf16_string_from_url_ascii(url.serialize(URL::ExcludeFragment::Yes));
    return blob_url_store().get(url_string);
}

}
