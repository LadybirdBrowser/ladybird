/*
 * Copyright (c) 2024, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Script.h>
#include <LibWeb/Bindings/CompressionStreamPrototype.h>
#include <LibWeb/Bindings/DecompressionStreamPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/Streams/WritableStream.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Compression {

class DecompressionStream final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(DecompressionStream, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(DecompressionStream);

public:
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<DecompressionStream>> construct_impl(JS::Realm&, Bindings::CompressionFormat format);
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<JS::Uint8Array>> decompress(JS::VM& vm, Bindings::CompressionFormat format, JS::Handle<WebIDL::BufferSource> buffer_source);
    virtual ~DecompressionStream() override;

    JS::GCPtr<Web::Streams::ReadableStream> readable() const
    {
        auto readable = MUST(m_this_value->get(JS::PropertyKey { "readable" }));
        return verify_cast<Web::Streams::ReadableStream>(readable.as_object());
    }

    JS::GCPtr<Web::Streams::WritableStream> writable() const
    {
        auto writable = MUST(m_this_value->get(JS::PropertyKey { "writable" }));
        return verify_cast<Web::Streams::WritableStream>(writable.as_object());
    }

private:
    DecompressionStream(JS::Realm&, Bindings::CompressionFormat);
    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    Bindings::CompressionFormat m_format;
    JS::GCPtr<JS::Script> m_js_script;
    JS::GCPtr<JS::Object> m_this_value;
};

}
