/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/ReadableStream.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/AsyncIterator.h>

namespace Web::Streams {

class ReadableStreamAsyncIterator final : public WebIDL::AsyncIterator {
    JS_OBJECT(ReadableStreamAsyncIterator, WebIDL::AsyncIterator);
    GC_DECLARE_ALLOCATOR(ReadableStreamAsyncIterator);

public:
    static WebIDL::ExceptionOr<GC::Ref<ReadableStreamAsyncIterator>> create(JS::Realm&, JS::Object::PropertyKind, ReadableStream&, Bindings::ReadableStreamIteratorOptions);

    virtual ~ReadableStreamAsyncIterator() override;

private:
    ReadableStreamAsyncIterator(JS::Realm&, JS::Object::PropertyKind, GC::Ref<ReadableStreamDefaultReader>, bool prevent_cancel);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(GC::Cell::Visitor&) override;

    virtual GC::Ref<WebIDL::Promise> next_iteration_result(JS::Realm&) override;
    virtual GC::Ref<WebIDL::Promise> iterator_return(JS::Realm&, JS::Value) override;

    GC::Ref<ReadableStreamDefaultReader> m_reader;
    bool m_prevent_cancel { false };
};

}
