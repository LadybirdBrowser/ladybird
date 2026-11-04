/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// The Event of a new StorageEvent in the realm whose type is the empty string, for createEvent().
GC::Ref<DOM::Event> create_storage_event(JS::Realm&);

}
