/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <AK/Utf16View.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/Wrappable.h>

namespace Web::DOM {

class DOMImplementation final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(DOMImplementation, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(DOMImplementation);

public:
    [[nodiscard]] static GC::Ref<DOMImplementation> create(Document&);
    virtual ~DOMImplementation();

    WebIDL::ExceptionOr<GC::Ref<XMLDocument>> create_document(Optional<Utf16FlyString> const&, Utf16FlyString const&, GC::Ptr<DocumentType>);
    GC::Ref<Document> create_html_document(Optional<Utf16String> const& title);
    WebIDL::ExceptionOr<GC::Ref<DocumentType>> create_document_type(Utf16FlyString const& name, Utf16View public_id, Utf16View system_id);

    // https://dom.spec.whatwg.org/#dom-domimplementation-hasfeature
    bool has_feature() const
    {
        // The hasFeature() method steps are to return true.
        return true;
    }

private:
    explicit DOMImplementation(Document&);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    Document& document() { return m_document; }
    Document const& document() const { return m_document; }

    GC::Ref<Document> m_document;
};

}
