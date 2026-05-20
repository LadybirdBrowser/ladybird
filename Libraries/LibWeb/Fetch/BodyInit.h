/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Variant.h>
#include <LibCore/ImmutableBytes.h>
#include <LibJS/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::Fetch {

// https://fetch.spec.whatwg.org/#bodyinit
using BodyInit = Variant<GC::Ref<Streams::ReadableStream>, GC::Ref<FileAPI::Blob>, GC::Ref<WebIDL::BufferSource>, GC::Ref<XHR::FormData>, GC::Ref<DOMURL::URLSearchParams>, String>;
using NullableBodyInit = Variant<GC::Ref<Streams::ReadableStream>, GC::Ref<FileAPI::Blob>, GC::Ref<WebIDL::BufferSource>, GC::Ref<XHR::FormData>, GC::Ref<DOMURL::URLSearchParams>, String, Empty>;

using BodyInitOrReadableBytes = Variant<GC::Ref<Streams::ReadableStream>, GC::Ref<FileAPI::Blob>, GC::Ref<WebIDL::BufferSource>, GC::Ref<XHR::FormData>, GC::Ref<DOMURL::URLSearchParams>, String, ReadonlyBytes, Core::ImmutableBytes>;
WEB_API Infrastructure::BodyWithType safely_extract_body(JS::Realm&, BodyInitOrReadableBytes const&);
WEB_API WebIDL::ExceptionOr<Infrastructure::BodyWithType> extract_body(JS::Realm&, BodyInitOrReadableBytes const&, bool keepalive = false);

}
