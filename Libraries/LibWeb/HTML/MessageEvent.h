/*
 * Copyright (c) 2021, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16FlyString.h>
#include <LibGC/Ptr.h>
#include <LibGC/RootVector.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Value.h>
#include <LibURL/Origin.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

struct MessageEventInit {
    JS::Value data { JS::js_null() };
    GC::RootVector<GC::Ref<MessagePort>> ports {};
    GC::Ptr<MessagePort> source;
};

// The Event of a new trusted MessageEvent in the realm, for the implementation to fire. Its origin is the origin if
// there is one, and the empty string otherwise.
WEB_API GC::Ref<DOM::Event> create_message_event(JS::Realm&, Utf16FlyString const& type, MessageEventInit const&, Optional<URL::Origin> const& origin = {});

// The Event of a new MessageEvent in the realm whose type is the empty string, for createEvent().
GC::Ref<DOM::Event> create_message_event(JS::Realm&);

}
