/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <LibWeb/DOM/Event.h>

namespace Web::WebGL {

class WebGLContextEvent final : public DOM::Event {
    WEB_PLATFORM_OBJECT(WebGLContextEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(WebGLContextEvent);

public:
    [[nodiscard]] static GC::Ref<WebGLContextEvent> create(JS::Realm&, Utf16FlyString const& type, Bindings::WebGLContextEventInit const&);
    static WebIDL::ExceptionOr<GC::Ref<WebGLContextEvent>> construct_impl(JS::Realm&, Utf16FlyString const& type, Bindings::WebGLContextEventInit const&);

    virtual ~WebGLContextEvent() override;

    Utf16String const& status_message() const { return m_status_message; }

private:
    WebGLContextEvent(JS::Realm&, Utf16FlyString const& type, Bindings::WebGLContextEventInit const&);

    virtual void initialize(JS::Realm&) override;

    Utf16String m_status_message;
};

}
