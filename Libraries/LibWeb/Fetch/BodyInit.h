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
#include <LibWeb/WebIDL/Buffers.h>

namespace Web::Fetch {

// https://fetch.spec.whatwg.org/#bodyinit
using XMLHttpRequestBodyInit = FlattenVariant<WebIDL::BufferSourceVariant, Variant<GC::Ref<FileAPI::Blob>, GC::Ref<XHR::FormData>, GC::Ref<DOMURL::URLSearchParams>, String>>;
using BodyInit = FlattenVariant<Variant<GC::Ref<Streams::ReadableStream>>, XMLHttpRequestBodyInit>;
using NullableBodyInit = FlattenVariant<BodyInit, Variant<Empty>>;

using BodyInitOrReadableBytes = FlattenVariant<BodyInit, Variant<ReadonlyBytes, Core::ImmutableBytes>>;
WEB_API Infrastructure::BodyWithType safely_extract_body(JS::Realm&, BodyInitOrReadableBytes const&);
WEB_API WebIDL::ExceptionOr<Infrastructure::BodyWithType> extract_body(JS::Realm&, BodyInitOrReadableBytes const&, bool keepalive = false);

}
