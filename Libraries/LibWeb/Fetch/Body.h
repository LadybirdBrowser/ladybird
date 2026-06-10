/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Forward.h>

namespace Web::Fetch {

struct MultiPartFormDataHeader {
    Optional<String> name;
    Optional<String> filename;
    Optional<String> content_type;
};

struct ContentDispositionHeader {
    String type;
    OrderedHashMap<String, String> parameters;
};

struct MultipartParsingError {
    String message;
};

template<typename T>
using MultipartParsingErrorOr = ErrorOr<T, MultipartParsingError>;

// https://fetch.spec.whatwg.org/#body-mixin
class BodyMixin {
public:
    virtual ~BodyMixin();

    virtual Optional<MimeSniff::MimeType> mime_type_impl() const = 0;
    virtual GC::Ptr<Infrastructure::Body> body_impl() = 0;
    virtual GC::Ptr<Infrastructure::Body const> body_impl() const = 0;

    [[nodiscard]] bool is_unusable() const;
    [[nodiscard]] GC::Ptr<Streams::ReadableStream> body() const;
    [[nodiscard]] bool body_used() const;

    void array_buffer(JS::Realm&, GC::Ref<WebIDL::Promise>) const;
    void blob(JS::Realm&, GC::Ref<WebIDL::Promise>) const;
    void bytes(JS::Realm&, GC::Ref<WebIDL::Promise>) const;
    void form_data(JS::Realm&, GC::Ref<WebIDL::Promise>) const;
    void json(JS::Realm&, GC::Ref<WebIDL::Promise>) const;
    void text(JS::Realm&, GC::Ref<WebIDL::Promise>) const;
};

[[nodiscard]] MultipartParsingErrorOr<GC::ConservativeVector<XHR::FormDataEntry>> parse_multipart_form_data(StringView input, MimeSniff::MimeType const& mime_type);

}
