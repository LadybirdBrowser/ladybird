/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/Forward.h>

namespace Web::PictureInPicture {

GC::Ref<DOM::Event> create_picture_in_picture_event(JS::Realm&, Utf16FlyString const& event_name, DOM::EventInit const&, PictureInPictureWindow& window);

}
