/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Utf16FlyString.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// The Event of a new trusted HashChangeEvent in the realm, for the implementation to fire.
GC::Ref<DOM::Event> create_hash_change_event(JS::Realm&, Utf16FlyString const& type, String const& old_url, String const& new_url);

// The Event of a new HashChangeEvent in the realm whose type is the empty string, for createEvent().
GC::Ref<DOM::Event> create_hash_change_event(JS::Realm&);

}
