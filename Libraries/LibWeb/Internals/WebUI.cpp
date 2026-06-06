/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Internals/WebUI.h>
#include <LibWeb/Page/Page.h>

namespace Web::Internals {

GC_DEFINE_ALLOCATOR(WebUI);

GC::Ref<Bindings::PlatformObject> WebUI::create(JS::Realm& realm)
{
    auto& window = HTML::relevant_window(realm.global_object());
    return Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, GC::Heap::the().allocate<WebUI>(window));
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
