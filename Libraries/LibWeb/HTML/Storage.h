/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024-2025, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Utf16String.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Bindings/Storage.h>
#include <LibWeb/Export.h>

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

    // A new Storage object of the type for the window, whose map is a new storage proxy map over the bottle for the
    // window's relevant settings object, or null when obtaining the bottle map fails.
    [[nodiscard]] static GC::Ptr<Storage> create(Window&, Type);

    // A window listening for storage events has its storage connected so that changes made in other processes reach
    // it; a map is otherwise only loaded on first access.
    void connect();

    void for_each_item(Function<void(Utf16String const& key, Utf16String const& value)> const&);

    void dump();
};

}
