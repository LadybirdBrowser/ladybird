/*
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Streams/ReadableByteStreamController.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::Streams {

// https://streams.spec.whatwg.org/#readablestreambyobrequest
class ReadableStreamBYOBRequest : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(ReadableStreamBYOBRequest, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(ReadableStreamBYOBRequest);

public:
    virtual ~ReadableStreamBYOBRequest() override = default;

    JS::GCPtr<WebIDL::ArrayBufferView> view();

    void set_controller(JS::GCPtr<ReadableByteStreamController> value) { m_controller = value; }

    void set_view(JS::GCPtr<WebIDL::ArrayBufferView> value) { m_view = value; }

    WebIDL::ExceptionOr<void> respond(WebIDL::UnsignedLongLong bytes_written);
    WebIDL::ExceptionOr<void> respond_with_new_view(JS::Handle<WebIDL::ArrayBufferView> const& view);

private:
    explicit ReadableStreamBYOBRequest(JS::Realm&);

    virtual void initialize(JS::Realm&) override;

    virtual void visit_edges(Cell::Visitor&) override;

    // https://streams.spec.whatwg.org/#readablestreambyobrequest-controller
    // The parent ReadableByteStreamController instance
    JS::GCPtr<ReadableByteStreamController> m_controller;

    // https://streams.spec.whatwg.org/#readablestreambyobrequest-view
    // A typed array representing the destination region to which the controller can write generated data, or null after the BYOB request has been invalidated.
    JS::GCPtr<WebIDL::ArrayBufferView> m_view;
};

}
