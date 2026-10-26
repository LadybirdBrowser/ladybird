/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024-2025, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/IDBFactory.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::IndexedDB {

// https://w3c.github.io/IndexedDB/#idbfactory
// The C++ side of an IDBFactory, which is implemented in Rust (Libraries/LibWeb/Bindings/Rust/src/indexed_db).
class WEB_API IDBFactory : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(IDBFactory, idb_factory)

public:
    [[nodiscard]] static GC::Ref<IDBFactory> create(JS::Realm&);
};

// https://w3c.github.io/IndexedDB/#cleanup-indexed-database-transactions
bool cleanup_indexed_database_transactions(HTML::EventLoop&);

// Runs close a database connection with every connection of the realm's global object whose close pending flag is
// unset.
void close_all_database_connections(JS::Realm&);

}
