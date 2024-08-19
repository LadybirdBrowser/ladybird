/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Function.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>

namespace Web::DOM {

class DocumentObserver final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(DocumentObserver, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(DocumentObserver);

public:
    [[nodiscard]] GC::Ptr<GC::Function<void()>> document_became_inactive() const { return m_document_became_inactive; }
    void set_document_became_inactive(Function<void()>);

    [[nodiscard]] GC::Ptr<GC::Function<void()>> document_completely_loaded() const { return m_document_completely_loaded; }
    void set_document_completely_loaded(Function<void()>);

private:
    explicit DocumentObserver(JS::Realm&, DOM::Document&);

    virtual void visit_edges(Cell::Visitor&) override;
    virtual void finalize() override;

    GC::Ref<DOM::Document> m_document;
    GC::Ptr<GC::Function<void()>> m_document_became_inactive;
    GC::Ptr<GC::Function<void()>> m_document_completely_loaded;
};

}
