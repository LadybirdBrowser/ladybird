/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibGC/Forward.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Bindings/OffscreenCanvas.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/OffscreenCanvas.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::HTML {

class Location;

}

namespace Web::Bindings {

WEB_API JS::Value location_wrapper(JS::Realm&, GC::Ref<HTML::Location>);

WEB_API WebIDL::ExceptionOr<GC::Ref<HTML::OffscreenCanvas>> construct_offscreen_canvas(JS::Realm&, WebIDL::UnsignedLong width, WebIDL::UnsignedLong height);
WEB_API GC::Ref<WebIDL::Promise> convert_to_blob(JS::Realm&, HTML::OffscreenCanvas&, Optional<ImageEncodeOptions> const&);
WEB_API JS::ThrowCompletionOr<HTML::OffscreenRenderingContext> get_context(JS::Realm&, HTML::OffscreenCanvas&, OffscreenRenderingContextId, JS::Value options);

}
