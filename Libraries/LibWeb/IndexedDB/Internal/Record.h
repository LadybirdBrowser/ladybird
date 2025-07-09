/*
 * Copyright (c) 2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibWeb/HTML/StructuredSerializeTypes.h>
#include <LibWeb/IndexedDB/Internal/Key.h>

namespace Web::IndexedDB {

// https://w3c.github.io/IndexedDB/#object-store-record
struct ObjectStoreRecord {
    GC::Ref<Key> key;
    HTML::SerializationRecord value;
};

// https://w3c.github.io/IndexedDB/#index-list-of-records
struct IndexRecord {
    GC::Ref<Key> key;
    GC::Ref<Key> value;
};

}