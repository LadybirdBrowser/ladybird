/*
 * Copyright (c) 2024-2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibWeb/Forward.h>
#include <LibWeb/StorageAPI/StorageKey.h>
#include <LibWeb/StorageAPI/StorageShelf.h>
#include <LibWeb/StorageAPI/StorageType.h>

namespace Web::StorageAPI {

// https://storage.spec.whatwg.org/#storage-shed
// A storage shed is a map of storage keys to storage shelves. It is initially empty.
class StorageShed {
public:
    Optional<StorageShelf&> obtain_a_storage_shelf(HTML::EnvironmentSettingsObject const&, StorageType);

private:
    OrderedHashMap<StorageKey, StorageShelf> m_data;
};

StorageShed& user_agent_storage_shed();

}
