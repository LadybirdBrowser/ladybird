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

    // 2. If key’s origin is an opaque origin, then return failure.
    if (key.origin.is_opaque())
        return {};

    // FIXME: 3. If the user has disabled storage, then return failure.

    // 4. Return key.
    return key;
}

// https://storage.spec.whatwg.org/#obtain-a-storage-key-for-non-storage-purposes
StorageKey obtain_a_storage_key_for_non_storage_purposes(HTML::Environment const& environment)
{
    // 1. Let origin be environment’s origin if environment is an environment settings object; otherwise environment’s creation URL’s origin.
    if (is<HTML::EnvironmentSettingsObject>(environment)) {
        auto const& settings = static_cast<HTML::EnvironmentSettingsObject const&>(environment);
        // FIXME: EnvironmentSettingsObject::origin() should be const :|
        auto& mutable_settings = const_cast<HTML::EnvironmentSettingsObject&>(settings);
        return { mutable_settings.origin() };
    }
    return { environment.creation_url.origin() };
}

}
