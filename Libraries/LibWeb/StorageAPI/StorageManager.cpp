/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/StorageAPI/StorageManager.h>

namespace Web::StorageAPI {

extern "C" {

// Defined in Bindings/Rust/src/storage_api/exports.rs.
void* web_rust_storage_api_create_storage_manager(JSVM*, void* realm);
}

GC::Ref<StorageManager> StorageManager::create(JS::Realm& realm)
{
    return *static_cast<StorageManager*>(web_rust_storage_api_create_storage_manager(JS::HostABI::vm_to_abi(realm.vm()), &realm));
}

}
