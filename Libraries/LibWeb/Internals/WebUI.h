/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Value.h>
#include <LibWeb/Internals/InternalsBase.h>

namespace Web::Internals {

class WebUI final : public InternalsBase {
    WEB_PLATFORM_OBJECT(WebUI, InternalsBase);
    GC_DECLARE_ALLOCATOR(WebUI);

public:
    virtual ~WebUI() override;

    void send_message(String const& name, JS::Value data);

private:
    explicit WebUI(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

}
