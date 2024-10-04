/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Heap/GCPtr.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/DOM/Document.h>

namespace Web::DOM {

class DOMImplementation final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(DOMImplementation, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(DOMImplementation);

public:
    [[nodiscard]] static JS::NonnullGCPtr<DOMImplementation> create(Document&);
    virtual ~DOMImplementation();

    WebIDL::ExceptionOr<JS::NonnullGCPtr<XMLDocument>> create_document(Optional<FlyString> const&, String const&, JS::GCPtr<DocumentType>) const;
    JS::NonnullGCPtr<Document> create_html_document(Optional<String> const& title) const;
    WebIDL::ExceptionOr<JS::NonnullGCPtr<DocumentType>> create_document_type(String const& qualified_name, String const& public_id, String const& system_id);

    // https://dom.spec.whatwg.org/#dom-domimplementation-hasfeature
    bool has_feature() const
    {
        // The hasFeature() method steps are to return true.
        return true;
    }

private:
    explicit DOMImplementation(Document&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    Document& document() { return m_document; }
    Document const& document() const { return m_document; }

    JS::NonnullGCPtr<Document> m_document;
};

}
