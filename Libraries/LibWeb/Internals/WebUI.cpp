/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Internals/WebUI.h>
#include <LibWeb/Page/Page.h>

namespace Web::Internals {

GC_DEFINE_ALLOCATOR(WebUI);

GC::Ref<Bindings::PlatformObject> WebUI::create(JS::Realm& realm)
{
    return Bindings::wrap(realm, realm.create<WebUI>(realm));
}

WebUI::WebUI(JS::Realm& realm)
    : InternalsBase(realm)
{
}

WebUI::~WebUI() = default;

void WebUI::send_message(String const& name, JS::Value data)
{
    page().client().received_message_from_web_ui(name, data);
}

}
