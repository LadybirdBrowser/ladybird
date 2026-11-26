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
}

GC::Ref<DataTransfer> DataTransfer::create(DragDataStore const& store)
{
    return *web_rust_data_transfer_create(JS::HostABI::vm_to_abi(JS::VM::the()), store.rust_store());
}

}
