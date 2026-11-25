/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/HTML/MimeTypeArray.h>

namespace Web::HTML {

// Defined in Bindings/Rust/src/html/mime_type_array.rs.
extern "C" void* web_rust_mime_type_array_create(JSVM*, Window*);

GC::Ref<MimeTypeArray> MimeTypeArray::create(Window& window)
{
    return GC::Ref { *static_cast<MimeTypeArray*>(web_rust_mime_type_array_create(JS::HostABI::vm_to_abi(JS::VM::the()), &window)) };
}

}
