/*
 * Copyright (c) 2024-2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Forward.h>
#include <LibWeb/StorageAPI/StorageKey.h>
#include <LibWeb/StorageAPI/StorageShelf.h>
#include <LibWeb/StorageAPI/StorageType.h>

namespace Web::StorageAPI {

// https://storage.spec.whatwg.org/#storage-shed
// A storage shed is a map of storage keys to storage shelves. It is initially empty.
class StorageShed : public GC::Cell {
    GC_CELL(StorageShed, GC::Cell);
    GC_DECLARE_ALLOCATOR(StorageShed);

public:
    static GC::Ref<StorageShed> create(GC::Heap& heap) { return heap.allocate<StorageShed>(); }

    GC::Ptr<StorageShelf> obtain_a_storage_shelf(HTML::EnvironmentSettingsObject&, StorageType);

    virtual void visit_edges(GC::Cell::Visitor& visitor) override;

private:
    StorageShed() = default;

    OrderedHashMap<StorageKey, GC::Ref<StorageShelf>> m_data;
};

}
