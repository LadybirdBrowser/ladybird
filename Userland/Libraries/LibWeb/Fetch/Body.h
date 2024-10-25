/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/GCPtr.h>
#include <LibWeb/Forward.h>

namespace Web::Fetch {

enum class PackageDataType {
    ArrayBuffer,
    Blob,
    Uint8Array,
    FormData,
    JSON,
    Text,
};

// https://fetch.spec.whatwg.org/#body-mixin
class BodyMixin {
public:
    virtual ~BodyMixin();

    virtual Optional<MimeSniff::MimeType> mime_type_impl() const = 0;
    virtual JS::GCPtr<Infrastructure::Body> body_impl() = 0;
    virtual JS::GCPtr<Infrastructure::Body const> body_impl() const = 0;
    virtual Bindings::PlatformObject& as_platform_object() = 0;
    virtual Bindings::PlatformObject const& as_platform_object() const = 0;

    [[nodiscard]] bool is_unusable() const;
    [[nodiscard]] JS::GCPtr<Streams::ReadableStream> body() const;
    [[nodiscard]] bool body_used() const;

    // JS API functions
    [[nodiscard]] WebIDL::ExceptionOr<JS::NonnullGCPtr<WebIDL::Promise>> array_buffer() const;
    [[nodiscard]] WebIDL::ExceptionOr<JS::NonnullGCPtr<WebIDL::Promise>> blob() const;
    [[nodiscard]] WebIDL::ExceptionOr<JS::NonnullGCPtr<WebIDL::Promise>> bytes() const;
    [[nodiscard]] WebIDL::ExceptionOr<JS::NonnullGCPtr<WebIDL::Promise>> form_data() const;
    [[nodiscard]] WebIDL::ExceptionOr<JS::NonnullGCPtr<WebIDL::Promise>> json() const;
    [[nodiscard]] WebIDL::ExceptionOr<JS::NonnullGCPtr<WebIDL::Promise>> text() const;
};

[[nodiscard]] WebIDL::ExceptionOr<JS::Value> package_data(JS::Realm&, ByteBuffer, PackageDataType, Optional<MimeSniff::MimeType> const&);
[[nodiscard]] WebIDL::ExceptionOr<JS::NonnullGCPtr<WebIDL::Promise>> consume_body(JS::Realm&, BodyMixin const&, PackageDataType);

}
