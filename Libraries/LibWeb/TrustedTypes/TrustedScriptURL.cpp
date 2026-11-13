/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/TrustedTypes/TrustedScriptURL.h>

namespace Web::TrustedTypes {

extern "C" {
// Defined in Bindings/Rust/src/trusted_types/trusted_script_url.rs.
TrustedScriptURL* web_rust_trusted_types_create_trusted_script_url(JSVM*, FlatPtr data);
FlatPtr web_rust_trusted_types_trusted_script_url_data(TrustedScriptURL*);
}

GC::Ref<TrustedScriptURL> TrustedScriptURL::create(Utf16String data)
{
    return *web_rust_trusted_types_create_trusted_script_url(JS::HostABI::vm_to_abi(JS::VM::the()), move(data).into_raw());
}

Utf16String TrustedScriptURL::to_string() const
{
    return Utf16String::from_raw(web_rust_trusted_types_trusted_script_url_data(const_cast<TrustedScriptURL*>(this)));
}

}
