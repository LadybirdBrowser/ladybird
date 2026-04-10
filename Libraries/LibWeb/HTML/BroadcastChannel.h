/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2024-2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/EventTarget.h>

namespace Web::HTML {

class BroadcastChannelRepository;

class BroadcastChannel final : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(BroadcastChannel, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(BroadcastChannel);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    [[nodiscard]] static GC::Ref<BroadcastChannel> construct_impl(JS::Realm&, FlyString const& name);

    // https://html.spec.whatwg.org/multipage/web-messaging.html#dom-broadcastchannel-name
    FlyString const& name() const
    {
        // The name getter steps are to return this's channel name.
        return m_channel_name;
    }

    WebIDL::ExceptionOr<void> post_message(JS::Value message);

    void close();

    void set_onmessage(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onmessage();
    void set_onmessageerror(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onmessageerror();

    static WEB_API void deliver_message_locally(BroadcastChannelMessage const&);

private:
    friend class BroadcastChannelRepository;

    BroadcastChannel(JS::Realm&, FlyString const& name);

    virtual void initialize(JS::Realm&) override;
    virtual void finalize() override;

    bool is_eligible_for_messaging() const;

    FlyString m_channel_name;
    u64 m_channel_id { 0 };
    bool m_closed_flag { false };
};

}
