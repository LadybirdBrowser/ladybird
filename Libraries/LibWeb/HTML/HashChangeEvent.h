/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Event.h>

namespace Web::HTML {

struct HashChangeEventInit : public DOM::EventInit {
    String old_url;
    String new_url;
};

class HashChangeEvent final : public DOM::Event {
    WEB_PLATFORM_OBJECT(HashChangeEvent, DOM::Event);
    JS_DECLARE_ALLOCATOR(HashChangeEvent);

public:
    [[nodiscard]] static JS::NonnullGCPtr<HashChangeEvent> create(JS::Realm&, FlyString const& event_name, HashChangeEventInit const&);
    [[nodiscard]] static JS::NonnullGCPtr<HashChangeEvent> construct_impl(JS::Realm&, FlyString const& event_name, HashChangeEventInit const&);

    String old_url() const { return m_old_url; }
    String new_url() const { return m_new_url; }

private:
    HashChangeEvent(JS::Realm&, FlyString const& event_name, HashChangeEventInit const& event_init);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(JS::Cell::Visitor& visitor) override;

    String m_old_url;
    String m_new_url;
};

}
