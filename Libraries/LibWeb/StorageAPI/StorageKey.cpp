/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/StorageAPI/StorageKey.h>

namespace Web::StorageAPI {

// https://storage.spec.whatwg.org/#obtain-a-storage-key
Optional<StorageKey> obtain_a_storage_key(HTML::Environment const& environment)
{
    // 1. Let key be the result of running obtain a storage key for non-storage purposes with environment.
    auto key = obtain_a_storage_key_for_non_storage_purposes(environment);

    // 2-4. The rest is the same for every origin; see the overload taking one.
    return obtain_a_storage_key(key.origin);
}

// https://storage.spec.whatwg.org/#obtain-a-storage-key-for-non-storage-purposes
StorageKey obtain_a_storage_key_for_non_storage_purposes(HTML::Environment const& environment)
{
    // 1. Let origin be environment’s origin if environment is an environment settings object; otherwise environment’s creation URL’s origin.
    auto origin = [&]() {
        if (auto const* settings = as_if<HTML::EnvironmentSettingsObject>(environment))
            return settings->origin();
        return environment.creation_url.origin();
    }();

    // 2. Return a tuple consisting of origin.
    return { move(origin) };
}

}
