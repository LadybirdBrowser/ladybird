/*
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/DOMStringMap.h>

namespace Web::HTML {

// Defined in Bindings/Rust/src/html/dom_string_map.rs.
extern "C" void* web_dom_string_map_create(JSVM*, DOM::Element*);

GC::Ref<DOMStringMap> DOMStringMap::create(DOM::Element& element)
{
    return GC::Ref { *static_cast<DOMStringMap*>(web_dom_string_map_create(JS::HostABI::vm_to_abi(JS::VM::the()), &element)) };
}

}
