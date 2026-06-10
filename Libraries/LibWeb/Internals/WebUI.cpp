/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Internals/WebUI.h>
#include <LibWeb/Page/Page.h>

namespace Web::Internals {

GC_DEFINE_ALLOCATOR(WebUI);

GC::Ref<WebUI> WebUI::create(HTML::Window& window)
{
    return GC::Heap::the().allocate<WebUI>(window);
}

WebUI::WebUI(HTML::Window& window)
    : InternalsBase(window)
{
}

WebUI::~WebUI() = default;

void WebUI::send_message(String const& name, JS::Value data)
{
    page().client().received_message_from_web_ui(name, data);
}

}
