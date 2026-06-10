/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ParentNode.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::DOM {

class WEB_API DocumentFragment
    : public ParentNode {
    WEB_WRAPPABLE(DocumentFragment, ParentNode);
    GC_DECLARE_ALLOCATOR(DocumentFragment);

public:
    [[nodiscard]] static GC::Ref<DocumentFragment> create(Document&);
    [[nodiscard]] static GC::Ref<DocumentFragment> construct_impl(JS::Realm&);

    virtual ~DocumentFragment() override = default;

    virtual FlyString node_name() const override { return "#document-fragment"_fly_string; }

    Element* host() { return m_host.ptr(); }
    Element const* host() const { return m_host.ptr(); }

    void set_host(Element*);

protected:
    explicit DocumentFragment(Document& document);

    virtual void visit_edges(Cell::Visitor&) override;

private:
    // https://dom.spec.whatwg.org/#concept-documentfragment-host
    GC::Ptr<Element> m_host;
};

template<>
inline bool Node::fast_is<DocumentFragment>() const { return is_document_fragment(); }

}
