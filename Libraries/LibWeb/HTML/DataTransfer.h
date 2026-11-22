/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/DataTransfer.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/HTML/DragDataStore.h>

namespace Web::HTML {

#define ENUMERATE_DATA_TRANSFER_EFFECTS        \
    __ENUMERATE_DATA_TRANSFER_EFFECT(none)     \
    __ENUMERATE_DATA_TRANSFER_EFFECT(copy)     \
    __ENUMERATE_DATA_TRANSFER_EFFECT(copyLink) \
    __ENUMERATE_DATA_TRANSFER_EFFECT(copyMove) \
    __ENUMERATE_DATA_TRANSFER_EFFECT(link)     \
    __ENUMERATE_DATA_TRANSFER_EFFECT(linkMove) \
    __ENUMERATE_DATA_TRANSFER_EFFECT(move)     \
    __ENUMERATE_DATA_TRANSFER_EFFECT(all)      \
    __ENUMERATE_DATA_TRANSFER_EFFECT(uninitialized)

namespace DataTransferEffect {

#define __ENUMERATE_DATA_TRANSFER_EFFECT(name) extern Utf16FlyString const& name;
ENUMERATE_DATA_TRANSFER_EFFECTS
#undef __ENUMERATE_DATA_TRANSFER_EFFECT

}

class DataTransfer final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(DataTransfer, data_transfer)

public:
    static GC::Ref<DataTransfer> create(NonnullRefPtr<DragDataStore>);

    Utf16FlyString drop_effect() const;
    void set_drop_effect(Utf16FlyString const&);
    Utf16FlyString effect_allowed() const;
    void set_effect_allowed_internal(Utf16FlyString const&);

    void disassociate_with_drag_data_store();
};

}
