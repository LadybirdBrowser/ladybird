/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Variant.h>
#include <LibJS/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::Fetch {

// https://fetch.spec.whatwg.org/#bodyinit
using BodyInit = Variant<GC::Root<Streams::ReadableStream>, GC::Root<FileAPI::Blob>, GC::Root<WebIDL::BufferSource>, GC::Root<XHR::FormData>, GC::Root<DOMURL::URLSearchParams>, String>;

using BodyInitOrReadableBytes = Variant<GC::Root<Streams::ReadableStream>, GC::Root<FileAPI::Blob>, GC::Root<WebIDL::BufferSource>, GC::Root<XHR::FormData>, GC::Root<DOMURL::URLSearchParams>, String, ReadonlyBytes>;
WEB_API Infrastructure::BodyWithType safely_extract_body(JS::Realm&, BodyInitOrReadableBytes const&);
WEB_API WebIDL::ExceptionOr<Infrastructure::BodyWithType> extract_body(JS::Realm&, BodyInitOrReadableBytes const&, bool keepalive = false);

}
