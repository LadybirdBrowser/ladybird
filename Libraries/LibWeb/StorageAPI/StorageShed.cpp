/*
 * Copyright (c) 2024-2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/StorageAPI/StorageShed.h>

namespace Web::StorageAPI {

// https://storage.spec.whatwg.org/#obtain-a-storage-shelf
Optional<StorageShelf&> StorageShed::obtain_a_storage_shelf(HTML::EnvironmentSettingsObject const& environment, StorageType type)
{
    // 1. Let key be the result of running obtain a storage key with environment.
    auto key = obtain_a_storage_key(environment);

    // 2. If key is failure, then return failure.
    if (!key.has_value())
        return {};

    // 3. If shed[key] does not exist, then set shed[key] to the result of running create a storage shelf with type.
    // 4. Return shed[key].
    return m_data.ensure(key.value(), [type] {
        return StorageShelf { type };
    });
}

// https://storage.spec.whatwg.org/#user-agent-storage-shed
StorageShed& user_agent_storage_shed()
{
    // A user agent holds a storage shed, which is a storage shed. A user agent’s storage shed holds all local storage data.
    // FIXME: Storing this statically in memory is not the correct place or way of doing this!
    static StorageShed storage_shed;
    return storage_shed;
}

}
