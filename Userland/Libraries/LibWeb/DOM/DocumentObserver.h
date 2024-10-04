/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibJS/Heap/HeapFunction.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>

namespace Web::DOM {

class DocumentObserver final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(DocumentObserver, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(DocumentObserver);

public:
    [[nodiscard]] JS::GCPtr<JS::HeapFunction<void()>> document_became_inactive() const { return m_document_became_inactive; }
    void set_document_became_inactive(Function<void()>);

    [[nodiscard]] JS::GCPtr<JS::HeapFunction<void()>> document_completely_loaded() const { return m_document_completely_loaded; }
    void set_document_completely_loaded(Function<void()>);

private:
    explicit DocumentObserver(JS::Realm&, DOM::Document&);

    virtual void visit_edges(Cell::Visitor&) override;
    virtual void finalize() override;

    JS::NonnullGCPtr<DOM::Document> m_document;
    JS::GCPtr<JS::HeapFunction<void()>> m_document_became_inactive;
    JS::GCPtr<JS::HeapFunction<void()>> m_document_completely_loaded;
};

}
