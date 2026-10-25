/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebCommon/StorageAPI/StorageKey.h>

namespace Web::StorageAPI {

StorageKey obtain_a_storage_key_for_non_storage_purposes(URL::Origin const& origin)
{
    // NOTE: This function exists as there are cases where we don't have the full environment object, but we still need to obtain a storage key.
    return { origin };
}

// https://storage.spec.whatwg.org/#obtain-a-storage-key
Optional<StorageKey> obtain_a_storage_key(URL::Origin const& origin)
{
    // 1. Let key be the result of running obtain a storage key for non-storage purposes with environment.
    // NB: The caller ran step 1; this is its key's origin.
    auto key = obtain_a_storage_key_for_non_storage_purposes(origin);

    // AD-HOC: "file://" URLs are opaque, but other browsers support storage on "file://" URLs.
    if (key.origin.is_opaque_file_origin())
        key.origin = URL::Origin { "file"_string, String {}, {} };

    // 2. If key's origin is an opaque origin, then return failure.
    if (key.origin.is_opaque())
        return {};

    // FIXME: 3. If the user has disabled storage, then return failure.

    // 4. Return key.
    return key;
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::StorageAPI::StorageKey const& key)
{
    TRY(encoder.encode(key.origin));
    return {};
}

template<>
ErrorOr<Web::StorageAPI::StorageKey> decode(Decoder& decoder)
{
    auto origin = TRY(decoder.decode<URL::Origin>());
    return Web::StorageAPI::StorageKey { move(origin) };
}

}
