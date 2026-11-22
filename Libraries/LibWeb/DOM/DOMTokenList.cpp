/*
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/DOM/DOMTokenList.h>
#include <LibWeb/DOM/Element.h>

namespace Web::DOM {

extern "C" {

// Defined in Bindings/Rust/src/dom/dom_token_list.rs.
void* web_dom_token_list_create(JSVM*, Element*, FlatPtr attribute_name);
void web_dom_token_list_attribute_changed(DOMTokenList*, FlatPtr value);
void web_dom_token_list_add(DOMTokenList*, FlatPtr token);
void web_dom_token_list_remove(DOMTokenList*, FlatPtr token);
void web_dom_token_list_toggle(DOMTokenList*, FlatPtr token, bool force);
}

GC::Ref<DOMTokenList> DOMTokenList::create(Element& element, Utf16FlyString const& associated_attribute)
{
    return GC::Ref { *static_cast<DOMTokenList*>(web_dom_token_list_create(
        JS::HostABI::vm_to_abi(JS::VM::the()), &element, associated_attribute.raw_identity())) };
}

void DOMTokenList::associated_attribute_changed(Optional<Utf16String> const& value)
{
    web_dom_token_list_attribute_changed(this, value.has_value() ? value->to_raw_leaked() : 0);
}

void DOMTokenList::add(Utf16View token)
{
    web_dom_token_list_add(this, Utf16String::from_utf16(token).into_raw());
}

void DOMTokenList::remove(Utf16View token)
{
    web_dom_token_list_remove(this, Utf16String::from_utf16(token).into_raw());
}

void DOMTokenList::toggle(Utf16View token, bool force)
{
    web_dom_token_list_toggle(this, Utf16String::from_utf16(token).into_raw(), force);
}

}
