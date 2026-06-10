/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/String.h>
#include <LibWeb/Bindings/WebGLContextEvent.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::WebGL {

using WebGLContextEventInit = Bindings::WebGLContextEventInit;

class WebGLContextEvent final : public DOM::Event {
    WEB_WRAPPABLE(WebGLContextEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(WebGLContextEvent);

public:
    [[nodiscard]] static GC::Ref<WebGLContextEvent> create(FlyString const& type, WebGLContextEventInit const&, HighResolutionTime::DOMHighResTimeStamp);

    virtual ~WebGLContextEvent() override;

    String const& status_message() const { return m_status_message; }

private:
    WebGLContextEvent(FlyString const& type, WebGLContextEventInit const&, HighResolutionTime::DOMHighResTimeStamp);

    String m_status_message;
};

}
