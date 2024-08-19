/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Variant.h>
#include <LibJS/Forward.h>
#include <LibWeb/Forward.h>

namespace Web::Fetch {

// https://fetch.spec.whatwg.org/#bodyinit
using BodyInit = Variant<GC::Handle<Streams::ReadableStream>, GC::Handle<FileAPI::Blob>, GC::Handle<WebIDL::BufferSource>, GC::Handle<XHR::FormData>, GC::Handle<DOMURL::URLSearchParams>, String>;

using BodyInitOrReadableBytes = Variant<GC::Handle<Streams::ReadableStream>, GC::Handle<FileAPI::Blob>, GC::Handle<WebIDL::BufferSource>, GC::Handle<XHR::FormData>, GC::Handle<DOMURL::URLSearchParams>, String, ReadonlyBytes>;
WebIDL::ExceptionOr<Infrastructure::BodyWithType> safely_extract_body(JS::Realm&, BodyInitOrReadableBytes const&);
WebIDL::ExceptionOr<Infrastructure::BodyWithType> extract_body(JS::Realm&, BodyInitOrReadableBytes const&, bool keepalive = false);

}
