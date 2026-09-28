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
