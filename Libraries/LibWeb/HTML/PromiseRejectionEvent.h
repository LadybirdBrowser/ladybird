/*
 * Copyright (c) 2021, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// The Event of a new trusted PromiseRejectionEvent in the realm, for the implementation to fire.
GC::Ref<DOM::Event> create_promise_rejection_event(JS::Realm&, Utf16FlyString const& type, DOM::EventInit const&, JS::Object& promise, JS::Value reason);

}
