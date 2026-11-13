/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/TrustedTypes/TrustedHTML.h>

namespace Web::TrustedTypes {

extern "C" {
// Defined in Bindings/Rust/src/trusted_types/trusted_html.rs.
TrustedHTML* web_rust_trusted_types_create_trusted_html(JSVM*, FlatPtr data);
FlatPtr web_rust_trusted_types_trusted_html_data(TrustedHTML*);
}

GC::Ref<TrustedHTML> TrustedHTML::create(Utf16String data)
{
    return *web_rust_trusted_types_create_trusted_html(JS::HostABI::vm_to_abi(JS::VM::the()), move(data).into_raw());
}

Utf16String TrustedHTML::to_string() const
{
    return Utf16String::from_raw(web_rust_trusted_types_trusted_html_data(const_cast<TrustedHTML*>(this)));
}

}
