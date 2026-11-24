/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// The Event of a new trusted SubmitEvent in the realm, for the implementation to fire.
GC::Ref<DOM::Event> create_submit_event(JS::Realm&, Utf16FlyString const& type, DOM::EventInit const&, GC::Ptr<HTMLElement> submitter);

}
