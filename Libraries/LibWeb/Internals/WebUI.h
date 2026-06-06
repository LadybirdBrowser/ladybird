/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Value.h>
#include <LibWeb/Export.h>
#include <LibWeb/Internals/InternalsBase.h>

namespace Web::Internals {

class WEB_API WebUI final : public InternalsBase {
    WEB_WRAPPABLE(WebUI, InternalsBase);
    GC_DECLARE_ALLOCATOR(WebUI);

public:
    static GC::Ref<Bindings::PlatformObject> create(JS::Realm&);

    virtual ~WebUI() override;

    void send_message(String const& name, JS::Value data);

private:
    explicit WebUI(HTML::Window&);
};

}
