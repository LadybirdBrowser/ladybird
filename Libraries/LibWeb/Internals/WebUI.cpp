/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WebUIPrototype.h>
#include <LibWeb/Internals/WebUI.h>
#include <LibWeb/Page/Page.h>

namespace Web::Internals {

GC_DEFINE_ALLOCATOR(WebUI);

WebUI::WebUI(JS::Realm& realm)
    : InternalsBase(realm)
{
}

WebUI::~WebUI() = default;

void WebUI::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WebUI);
}

void WebUI::send_message(String const& name, JS::Value data)
{
    page().client().received_message_from_web_ui(name, data);
}

}
