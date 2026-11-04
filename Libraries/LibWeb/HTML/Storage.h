/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024-2025, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Utf16String.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Bindings/Storage.h>
#include <LibWeb/Export.h>
#include <LibWeb/StorageAPI/StorageBottle.h>

namespace Web::HTML {

class Window;

// https://html.spec.whatwg.org/multipage/webstorage.html#storage-2
class WEB_API Storage : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(Storage, storage)

public:
    // https://html.spec.whatwg.org/multipage/webstorage.html#concept-storage-type
    enum class Type : u8 {
        Local,
        Session,
    };

    [[nodiscard]] static GC::Ref<Storage> create(Window&, Type, GC::Ref<StorageAPI::StorageBottle>);

    StorageAPI::StorageBottle& map();

    void dump();

    // A window listening for storage events has its storage connected so that changes made in other processes reach
    // it; a map is otherwise only loaded on first access.
    void connect() { map().connect(); }

    // Fires the storage events that a change another process made calls for on every Storage object in this process
    // that shares the changed map.
    static void dispatch_storage_events_for_remote_change(Type, StorageAPI::StorageKey const&, Optional<PageId> session_page, Optional<Utf16String> const& key, Optional<Utf16String> const& old_value, Optional<Utf16String> const& new_value, String const& url);
};

}
