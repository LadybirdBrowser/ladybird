/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/HTML/DataTransfer.h>
#include <LibWeb/HTML/DragDataStore.h>

namespace Web::HTML {

extern "C" {

// Defined in Bindings/Rust/src/html/data_transfer.rs.
DataTransfer* web_rust_data_transfer_create(JSVM*, RustDragDataStore const*);
void web_rust_data_transfer_disassociate(DataTransfer*);
FlatPtr web_rust_data_transfer_drop_effect(DataTransfer const*);
FlatPtr web_rust_data_transfer_effect_allowed(DataTransfer const*);
void web_rust_data_transfer_set_drop_effect(DataTransfer*, FlatPtr);
void web_rust_data_transfer_set_effect_allowed_internal(DataTransfer*, FlatPtr);
}

namespace DataTransferEffect {

#define __ENUMERATE_DATA_TRANSFER_EFFECT(name) Utf16FlyString const& name = *new Utf16FlyString(#name##_utf16_fly_string);
ENUMERATE_DATA_TRANSFER_EFFECTS
#undef __ENUMERATE_DATA_TRANSFER_EFFECT

}

GC::Ref<DataTransfer> DataTransfer::create(DragDataStore const& store)
{
    return *web_rust_data_transfer_create(JS::HostABI::vm_to_abi(JS::VM::the()), store.rust_store());
}

Utf16FlyString DataTransfer::drop_effect() const
{
    return Utf16FlyString::adopt_raw(web_rust_data_transfer_drop_effect(this));
}

void DataTransfer::set_drop_effect(Utf16FlyString const& effect)
{
    web_rust_data_transfer_set_drop_effect(this, effect.raw_identity());
}

Utf16FlyString DataTransfer::effect_allowed() const
{
    return Utf16FlyString::adopt_raw(web_rust_data_transfer_effect_allowed(this));
}

void DataTransfer::set_effect_allowed_internal(Utf16FlyString const& effect)
{
    web_rust_data_transfer_set_effect_allowed_internal(this, effect.raw_identity());
}

void DataTransfer::disassociate_with_drag_data_store()
{
    web_rust_data_transfer_disassociate(this);
}

}
