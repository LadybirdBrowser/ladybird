/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/StorageAPI/StorageManager.h>

namespace Web::StorageAPI {

extern "C" {

// Defined in Bindings/Rust/src/storage_api/exports.rs.
void* web_rust_storage_api_create_storage_manager(JSVM*, void* realm);

// Declared on the Rust side in Bindings/Rust/src/storage_api/storage_shelf.rs.
u64 web_storage_api_storage_usage(HTML::EnvironmentSettingsObject*);
}

GC::Ref<StorageManager> StorageManager::create(JS::Realm& realm)
{
    return *static_cast<StorageManager*>(web_rust_storage_api_create_storage_manager(JS::HostABI::vm_to_abi(realm.vm()), &realm));
}

u64 web_storage_api_storage_usage(HTML::EnvironmentSettingsObject* environment)
{
    // FIXME: This should be implemented in a way that works for Workers.
    auto& window = HTML::relevant_window(environment->global_object());
    return window.page().client().page_did_request_storage_usage(environment->id);
}

}
