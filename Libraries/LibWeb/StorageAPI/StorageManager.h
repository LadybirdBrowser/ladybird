/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Bindings/StorageManager.h>

namespace Web::StorageAPI {

// https://storage.spec.whatwg.org/#storagemanager
// The C++ side of a StorageManager, which is implemented in Rust (Libraries/LibWeb/Bindings/Rust/src/storage_api).
class StorageManager : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(StorageManager, storage_manager)

public:
    [[nodiscard]] static GC::Ref<StorageManager> create(JS::Realm&);
};

}
