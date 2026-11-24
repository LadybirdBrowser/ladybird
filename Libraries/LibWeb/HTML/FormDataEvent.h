/*
 * Copyright (c) 2023, Kenneth Myhra <kennethmyhra@serenityos.org>
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

// The Event of a new trusted FormDataEvent in the realm, for the implementation to fire.
GC::Ref<DOM::Event> create_form_data_event(JS::Realm&, Utf16FlyString const& type, DOM::EventInit const&, XHR::FormData&);

}
