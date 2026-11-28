/*
 * Copyright (c) 2020-2021, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/DocumentFragment.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>

namespace Web::DOM {

static_assert(is_shell_of_rust_object<DocumentFragment, RustFFI::DocumentFragmentObject>);

DocumentFragment::DocumentFragment(VtableProbe probe)
    : ParentNode(probe)
    , m_document_fragment_core { .host = {}, .style_engine_tree_scope = {} }
{
    // NB: libweb_rust reads a fragment as the DocumentFragmentPrefix it starts with.
    static_assert(sizeof(DocumentFragment) == sizeof(RustFFI::DocumentFragmentPrefix));
    static_assert(offsetof(DocumentFragment, m_document_fragment_core) == offsetof(RustFFI::DocumentFragmentPrefix, core));

    // NB: The fields libweb_rust reads start after all of the base class, where every C++ ABI places them, and not in
    //     its tail padding, where only the Itanium ABI could.
    static_assert(offsetof(DocumentFragment, m_document_fragment_core) >= sizeof(ParentNode));
}

// NB: Leaves the core of the fragment as libweb_rust made it.
DocumentFragment::DocumentFragment(RustMadePrefix prefix)
    : ParentNode(prefix)
{
}

void DocumentFragment::set_host(Web::DOM::Element* element)
{
    // NB: libweb_rust reads the host as a node, so the element must start with its node, as every cell type starts with
    //     its cell.
    VERIFY(static_cast<void const*>(static_cast<Node const*>(element)) == static_cast<void const*>(element));
    m_document_fragment_core.host = element;
}

GC::Ref<DocumentFragment> DocumentFragment::create(Document& document)
{
    return static_cast<DocumentFragment&>(*RustFFI::rust_dom_create_document_fragment(&document));
}

GC::Ref<DocumentFragment> DocumentFragment::create_for_constructor(JS::Object& relevant_global_object)
{
    return create(HTML::relevant_window(relevant_global_object).associated_document());
}

}
