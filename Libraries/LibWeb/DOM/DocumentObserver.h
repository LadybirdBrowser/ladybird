/*
 * Copyright (c) 2023-2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Cell.h>
#include <LibGC/Function.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/DocumentReadyState.h>
#include <LibWeb/HTML/VisibilityState.h>

namespace Web::DOM {

class WEB_API DocumentObserver final : public GC::Cell {
    GC_CELL(DocumentObserver, GC::Cell);
    GC_DECLARE_ALLOCATOR(DocumentObserver);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    static GC::Ref<DocumentObserver> create(Document&);

    [[nodiscard]] GC::Ptr<GC::Function<void()>> document_became_active() const { return m_document_became_active; }
    void set_document_became_active(Function<void()>);

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

    GC::Ref<Document> document() { return m_document; }
    void set_document(GC::Ref<Document>);

private:
    explicit DocumentObserver(Document&);

    virtual void visit_edges(GC::Cell::Visitor&) override;
    virtual void finalize() override;

    GC::Ref<Document> m_document;
    GC::Ptr<GC::Function<void()>> m_document_became_active;
    GC::Ptr<GC::Function<void()>> m_document_became_inactive;
    GC::Ptr<GC::Function<void()>> m_document_completely_loaded;
    GC::Ptr<GC::Function<void(HTML::DocumentReadyState)>> m_document_readiness_observer;
    GC::Ptr<GC::Function<void(HTML::VisibilityState)>> m_document_visibility_state_observer;
    GC::Ptr<GC::Function<void(bool)>> m_document_page_showing_observer;
};

}
