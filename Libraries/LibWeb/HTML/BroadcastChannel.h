/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/EventTarget.h>

namespace Web::HTML {

class BroadcastChannel final : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(BroadcastChannel, DOM::EventTarget);
    JS_DECLARE_ALLOCATOR(BroadcastChannel);

public:
    [[nodiscard]] static JS::NonnullGCPtr<BroadcastChannel> construct_impl(JS::Realm&, FlyString const& name);

    FlyString name();

    void close();

    void set_onmessage(WebIDL::CallbackType*);
    WebIDL::CallbackType* onmessage();
    void set_onmessageerror(WebIDL::CallbackType*);
    WebIDL::CallbackType* onmessageerror();

private:
    BroadcastChannel(JS::Realm&, FlyString const& name);

    virtual void initialize(JS::Realm&) override;

    FlyString m_channel_name;
    bool m_closed_flag { false };
};

}
