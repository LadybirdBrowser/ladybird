/*
 * Copyright (c) 2023-2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Function.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/DocumentReadyState.h>
#include <LibWeb/HTML/VisibilityState.h>

namespace Web::DOM {

class DocumentObserver final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(DocumentObserver, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(DocumentObserver);

public:
    [[nodiscard]] GC::Ptr<GC::Function<void()>> document_became_inactive() const { return m_document_became_inactive; }
    void set_document_became_inactive(Function<void()>);

    [[nodiscard]] GC::Ptr<GC::Function<void()>> document_completely_loaded() const { return m_document_completely_loaded; }
    void set_document_completely_loaded(Function<void()>);

    [[nodiscard]] GC::Ptr<GC::Function<void(HTML::DocumentReadyState)>> document_readiness_observer() const { return m_document_readiness_observer; }
    void set_document_readiness_observer(Function<void(HTML::DocumentReadyState)>);

    [[nodiscard]] GC::Ptr<GC::Function<void(HTML::VisibilityState)>> document_visibility_state_observer() const { return m_document_visibility_state_observer; }
    void set_document_visibility_state_observer(Function<void(HTML::VisibilityState)>);

    [[nodiscard]] GC::Ptr<GC::Function<void(bool)>> document_page_showing_observer() const { return m_document_page_showing_observer; }
    void set_document_page_showing_observer(Function<void(bool)>);

private:
    explicit DocumentObserver(JS::Realm&, DOM::Document&);

    virtual void visit_edges(Cell::Visitor&) override;
    virtual void finalize() override;

    GC::Ref<DOM::Document> m_document;
    GC::Ptr<GC::Function<void()>> m_document_became_inactive;
    GC::Ptr<GC::Function<void()>> m_document_completely_loaded;
    GC::Ptr<GC::Function<void(HTML::DocumentReadyState)>> m_document_readiness_observer;
    GC::Ptr<GC::Function<void(HTML::VisibilityState)>> m_document_visibility_state_observer;
    GC::Ptr<GC::Function<void(bool)>> m_document_page_showing_observer;
};

}
