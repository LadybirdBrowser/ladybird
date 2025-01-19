/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024-2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>
#include <LibWeb/Crypto/Crypto.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/FileAPI/Blob.h>
#include <LibWeb/FileAPI/BlobURLStore.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/StorageAPI/StorageKey.h>

namespace Web::FileAPI {

BlobURLStore& blob_url_store()
{
    static HashMap<String, BlobURLEntry> store;
    return store;
}

// https://w3c.github.io/FileAPI/#unicodeBlobURL
ErrorOr<String> generate_new_blob_url()
{
    // 1. Let result be the empty string.
    StringBuilder result;

    // 2. Append the string "blob:" to result.
    TRY(result.try_append("blob:"sv));

    // 3. Let settings be the current settings object
    auto& settings = HTML::current_principal_settings_object();

    // 4. Let origin be settings’s origin.
    auto origin = settings.origin();

    // 5. Let serialized be the ASCII serialization of origin.
    auto serialized = origin.serialize();

    // 6. If serialized is "null", set it to an implementation-defined value.
    if (serialized == "null"sv)
        serialized = "ladybird"_string;

    // 7. Append serialized to result.
    TRY(result.try_append(serialized));

    // 8. Append U+0024 SOLIDUS (/) to result.
    TRY(result.try_append('/'));

    // 9. Generate a UUID [RFC4122] as a string and append it to result.
    auto uuid = TRY(Crypto::generate_random_uuid());
    TRY(result.try_append(uuid));

    // 10. Return result.
    return result.to_string();
}

// https://w3c.github.io/FileAPI/#add-an-entry
ErrorOr<String> add_entry_to_blob_url_store(GC::Ref<Blob> object)
{
    // 1. Let store be the user agent’s blob URL store.
    auto& store = blob_url_store();

    // 2. Let url be the result of generating a new blob URL.
    auto url = TRY(generate_new_blob_url());

    // 3. Let entry be a new blob URL entry consisting of object and the current settings object.
    BlobURLEntry entry { object, HTML::current_principal_settings_object() };

    // 4. Set store[url] to entry.
    TRY(store.try_set(url, move(entry)));

    // 5. Return url.
    return url;
}

// https://www.w3.org/TR/FileAPI/#check-for-same-partition-blob-url-usage
bool check_for_same_partition_blob_url_usage(URL::BlobURLEntry const& blob_url_entry, GC::Ref<HTML::Environment> environment)
{
    // 1. Let blobStorageKey be the result of obtaining a storage key for non-storage purposes with blobUrlEntry’s environment.
    auto blob_storage_key = StorageAPI::obtain_a_storage_key_for_non_storage_purposes(blob_url_entry.environment.origin);

    // 2. Let environmentStorageKey be the result of obtaining a storage key for non-storage purposes with environment.
    auto environment_storage_key = StorageAPI::obtain_a_storage_key_for_non_storage_purposes(environment);

    // 3. If blobStorageKey is not equal to environmentStorageKey, then return false.
    if (blob_storage_key != environment_storage_key)
        return false;

    // 4. Return true.
    return true;
}

// https://www.w3.org/TR/FileAPI/#blob-url-obtain-object
Optional<URL::BlobURLEntry::Object> obtain_a_blob_object(URL::BlobURLEntry const& blob_url_entry, Variant<GC::Ref<HTML::Environment>, NavigationEnvironment> environment)
{
    // 1. Let isAuthorized be true.
    bool is_authorized = true;

    // 2. If environment is not the string "navigation", then set isAuthorized to the result of checking for same-partition blob URL usage with blobUrlEntry and environment.
    if (!environment.has<NavigationEnvironment>())
        is_authorized = check_for_same_partition_blob_url_usage(blob_url_entry, environment.get<GC::Ref<HTML::Environment>>());

    // 3. If isAuthorized is false, then return failure.
    if (!is_authorized)
        return {};

    // 4. Return blobUrlEntry’s object.
    return blob_url_entry.object;
}

// https://w3c.github.io/FileAPI/#removeTheEntry
void remove_entry_from_blob_url_store(StringView url)
{
    // 1. Let store be the user agent’s blob URL store;
    auto& store = blob_url_store();

    // 2. Let url string be the result of serializing url.
    auto url_string = URL::URL { url }.to_string();

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
    store.remove_all_matching([&](auto&, auto& value) {
        return value.environment == &environment;
    });
}

// https://w3c.github.io/FileAPI/#blob-url-resolve
Optional<BlobURLEntry const&> resolve_a_blob_url(URL::URL const& url)
{
    // 1. Assert: url’s scheme is "blob".
    VERIFY(url.scheme() == "blob"sv);

    // 2. Let store be the user agent’s blob URL store.
    auto& store = blob_url_store();

    // 3. Let url string be the result of serializing url with the exclude fragment flag set.
    auto url_string = url.serialize(URL::ExcludeFragment::Yes);

    // 4. If store[url string] exists, return store[url string]; otherwise return failure.
    return store.get(url_string);
}

}
