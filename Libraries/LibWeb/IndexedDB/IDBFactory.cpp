/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/IndexedDB/IDBFactory.h>

// The C ABI of the Rust implementation of IndexedDB, Libraries/LibWeb/Bindings/Rust/src/indexed_db/exports.rs.
extern "C" {
void* web_rust_indexed_db_create_factory(JSVM*, void* realm);
bool web_rust_indexed_db_cleanup_indexed_database_transactions(JSVM*, void* event_loop);
void web_rust_indexed_db_close_all_connections(JSVM*, void* realm);
}

namespace Web::IndexedDB {

GC::Ref<IDBFactory> IDBFactory::create(JS::Realm& realm)
{
    return *static_cast<IDBFactory*>(web_rust_indexed_db_create_factory(JS::HostABI::vm_to_abi(realm.vm()), &realm));
}

bool cleanup_indexed_database_transactions(HTML::EventLoop& event_loop)
{
    return web_rust_indexed_db_cleanup_indexed_database_transactions(JS::HostABI::vm_to_abi(Bindings::main_thread_vm()), &event_loop);
}

void close_all_database_connections(JS::Realm& realm)
{
    web_rust_indexed_db_close_all_connections(JS::HostABI::vm_to_abi(realm.vm()), &realm);
}

}
