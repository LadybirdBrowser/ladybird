/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/HTML/DOMStringList.h>

namespace Web::HTML {

// Defined in Bindings/Rust/src/html/dom_string_list.rs.
extern "C" void* web_dom_string_list_create(JSVM*, ReadonlySpan<FlatPtr> strings);

GC::Ref<DOMStringList> DOMStringList::create(Vector<Utf16String> strings)
{
    Vector<FlatPtr> raw_strings;
    raw_strings.ensure_capacity(strings.size());
    for (auto& string : strings)
        raw_strings.unchecked_append(move(string).into_raw());

    return GC::Ref { *static_cast<DOMStringList*>(web_dom_string_list_create(JS::HostABI::vm_to_abi(JS::VM::the()), raw_strings.span())) };
}

}
