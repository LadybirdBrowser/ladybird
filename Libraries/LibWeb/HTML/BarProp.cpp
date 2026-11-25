/*
 * Copyright (c) 2025-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/HTML/BarProp.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/Window.h>

namespace Web::HTML {

extern "C" {
// Defined in Bindings/Rust/src/html/bar_prop.rs.
void* web_rust_bar_prop_create(JSVM*, Window*);

// Called by Bindings/Rust/src/html/bar_prop.rs.
BrowsingContext* web_html_bar_prop_window_browsing_context(Window*);
bool web_html_bar_prop_top_level_browsing_context_is_popup(BrowsingContext*);
}

GC::Ref<BarProp> BarProp::create(Window& window)
{
    return GC::Ref { *static_cast<BarProp*>(web_rust_bar_prop_create(JS::HostABI::vm_to_abi(JS::VM::the()), &window)) };
}

BrowsingContext* web_html_bar_prop_window_browsing_context(Window* window)
{
    return window->associated_document().browsing_context().ptr();
}

bool web_html_bar_prop_top_level_browsing_context_is_popup(BrowsingContext* browsing_context)
{
    auto top_level_browsing_context = browsing_context->top_level_browsing_context();
    return top_level_browsing_context && top_level_browsing_context->is_popup() == TokenizedFeature::Popup::Yes;
}

}
