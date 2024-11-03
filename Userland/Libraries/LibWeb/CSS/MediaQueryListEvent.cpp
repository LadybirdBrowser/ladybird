/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MediaQueryListEventPrototype.h>
#include <LibWeb/CSS/MediaQueryListEvent.h>

namespace Web::CSS {

JS_DEFINE_ALLOCATOR(MediaQueryListEvent);

JS::NonnullGCPtr<MediaQueryListEvent> MediaQueryListEvent::construct_impl(JS::Realm& realm, FlyString const& event_name, MediaQueryListEventInit const& event_init)
{
    return realm.heap().allocate<MediaQueryListEvent>(realm, realm, event_name, event_init);
}

MediaQueryListEvent::MediaQueryListEvent(JS::Realm& realm, FlyString const& event_name, MediaQueryListEventInit const& event_init)
    : DOM::Event(realm, event_name, event_init)
    , m_media(event_init.media)
    , m_matches(event_init.matches)
{
}

MediaQueryListEvent::~MediaQueryListEvent() = default;

void MediaQueryListEvent::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(MediaQueryListEvent);
}

}
