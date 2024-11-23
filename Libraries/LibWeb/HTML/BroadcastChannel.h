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
    GC_DECLARE_ALLOCATOR(BroadcastChannel);

public:
    [[nodiscard]] static GC::Ref<BroadcastChannel> construct_impl(JS::Realm&, FlyString const& name);

    // https://html.spec.whatwg.org/multipage/web-messaging.html#dom-broadcastchannel-name
    FlyString const& name() const
    {
        // The name getter steps are to return this's channel name.
        return m_channel_name;
    }

    void close();

    void set_onmessage(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onmessage();
    void set_onmessageerror(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onmessageerror();

private:
    BroadcastChannel(JS::Realm&, FlyString const& name);

    virtual void initialize(JS::Realm&) override;

    FlyString m_channel_name;
    bool m_closed_flag { false };
};

}
