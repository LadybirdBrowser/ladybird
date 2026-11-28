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
    [[nodiscard]] static GC::Ref<DocumentFragment> create_for_constructor(JS::Object&);

    virtual ~DocumentFragment() override = default;

    virtual Utf16FlyString node_name() const override { return "#document-fragment"_utf16_fly_string; }

    Element* host() { return m_document_fragment_core.host.ptr(); }
    Element const* host() const { return m_document_fragment_core.host.ptr(); }

    void set_host(Element*);

protected:
    explicit DocumentFragment(Document& document);

    RustFFI::DocumentFragmentCore m_document_fragment_core;
};

}
