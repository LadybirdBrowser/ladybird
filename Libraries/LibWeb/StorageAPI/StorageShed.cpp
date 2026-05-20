/*
 * Copyright (c) 2024-2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibGC/Heap.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/StorageAPI/StorageBottle.h>
#include <LibWeb/StorageAPI/StorageShed.h>

namespace Web::StorageAPI {

GC_DEFINE_ALLOCATOR(StorageShed);

void StorageShed::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_data);
}

// https://storage.spec.whatwg.org/#obtain-a-storage-shelf
GC::Ptr<StorageShelf> StorageShed::obtain_a_storage_shelf(HTML::EnvironmentSettingsObject& environment, StorageType type)
{
    // 1. Let key be the result of running obtain a storage key with environment.
    auto key = obtain_a_storage_key(environment);

    auto& page = as<HTML::Window>(environment.global_object()).page();

    // 2. If key is failure, then return failure.
    if (!key.has_value())
        return {};

    // 3. If shed[key] does not exist, then set shed[key] to the result of running create a storage shelf with type.
    // 4. Return shed[key].
    return m_data.ensure(key.value(), [&page, key, type, &heap = this->heap()] {
        return StorageShelf::create(heap, page, *key, type);
    });
}

// https://storage.spec.whatwg.org/#legacy-clone-a-traversable-storage-shed
// To legacy-clone a traversable storage shed, given a traversable navigable A and a traversable navigable B, run these steps:
void StorageShed::legacy_clone(StorageShed const& a_storage_shed, GC::Ref<Page> page)
{
    // 1. For each key → shelf of A’s storage shed:
    for (auto const& [key, shelf] : a_storage_shed.m_data) {

        // 1. Let newShelf be the result of running create a storage shelf with "session".
        auto new_shelf = StorageShelf::create(heap(), page, key, StorageType::Session);

        // 2. Set newShelf’s bucket map["default"]'s bottle map["sessionStorage"]'s map to a clone of shelf’s bucket map["default"]'s bottle map["sessionStorage"]'s map.
        auto& shelf_bucket = *shelf->bucket_map().get("default"sv).value();
        auto& new_shelf_bucket = *new_shelf->bucket_map().get("default"sv).value();

        auto const& shelf_bottle = *shelf_bucket.bottle_map()[to_underlying(StorageEndpointType::SessionStorage)];
        auto& new_shelf_bottle = *new_shelf_bucket.bottle_map()[to_underlying(StorageEndpointType::SessionStorage)];

        as<SessionStorageBottle>(new_shelf_bottle).copy_map_from(static_cast<SessionStorageBottle const&>(shelf_bottle));

        // 3. Set B’s storage shed[key] to newShelf.
        m_data.set(key, new_shelf);
    }
}

}
