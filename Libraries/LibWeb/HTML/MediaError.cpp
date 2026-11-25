/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/HTML/MediaError.h>

namespace Web::HTML {

extern "C" {
// Defined in Bindings/Rust/src/html/media_error.rs.
void* web_rust_media_error_create(JSVM*, u16 code, FlatPtr message);
u16 web_rust_media_error_code(Bindings::RustInterfaceCell const*);
FlatPtr web_rust_media_error_message(Bindings::RustInterfaceCell const*);
}

GC::Ref<MediaError> MediaError::create(Code code, Utf16String const& message)
{
    return GC::Ref { *static_cast<MediaError*>(web_rust_media_error_create(JS::HostABI::vm_to_abi(JS::VM::the()), to_underlying(code), message.raw_identity())) };
}

MediaError::Code MediaError::code() const
{
    return static_cast<Code>(web_rust_media_error_code(this));
}

Utf16String MediaError::message() const
{
    return Utf16String::adopt_raw(web_rust_media_error_message(this));
}

}
