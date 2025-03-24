/*
 * Copyright (c) 2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/IndexedDB/Internal/ObjectStore.h>

namespace Web::IndexedDB {

GC_DEFINE_ALLOCATOR(ObjectStore);

ObjectStore::~ObjectStore() = default;

GC::Ref<ObjectStore> ObjectStore::create(JS::Realm& realm, String name, bool auto_increment, Optional<KeyPath> const& key_path)
{
    return realm.create<ObjectStore>(name, auto_increment, key_path);
}

}
