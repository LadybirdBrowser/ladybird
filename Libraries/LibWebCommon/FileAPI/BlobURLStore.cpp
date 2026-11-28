/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebCommon/FileAPI/BlobURLStore.h>
#include <LibWebCommon/StorageAPI/StorageKey.h>

namespace Web::FileAPI {

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
Optional<SerializedBlobURLEntry::Object> obtain_a_blob_object(SerializedBlobURLEntry const& blob_url_entry, Variant<StorageAPI::StorageKey, TopLevelNavigation, TopLevelSelfFetch> environment)
{
    // 1. Let isAuthorized be true.
    bool is_authorized = true;

    // 2. If environment is an environment, then set isAuthorized to the result of checking for same-partition blob URL usage with blobUrlEntry and environment.
    // AD-HOC: An environment is given as its storage key for non-storage purposes, the only part of it that checking for
    //         same-partition blob URL usage reads.
    if (auto const* environment_storage_key = environment.get_pointer<StorageAPI::StorageKey>())
        is_authorized = check_for_same_partition_blob_url_usage(blob_url_entry.origin, environment_storage_key->origin);

    // 3. If isAuthorized is false, then return failure.
    if (!is_authorized)
        return {};

    // 4. Return blobUrlEntry’s object.
    return blob_url_entry.object;
}

}
