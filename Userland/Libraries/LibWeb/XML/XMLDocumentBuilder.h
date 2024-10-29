/*
 * Copyright (c) 2022, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Comment.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Namespace.h>
#include <LibXML/Parser/Parser.h>

namespace Web {

enum class XMLScriptingSupport {
    Disabled,
    Enabled,
};

ErrorOr<Variant<ByteString, Vector<XML::MarkupDeclaration>>> resolve_xml_resource(XML::SystemID const&, Optional<XML::PublicID> const&);

class XMLDocumentBuilder final : public XML::Listener {
public:
    XMLDocumentBuilder(DOM::Document& document, XMLScriptingSupport = XMLScriptingSupport::Enabled);

    bool has_error() const { return m_has_error; }

private:
    virtual void set_source(ByteString) override;
    virtual void set_doctype(XML::Doctype) override;
    virtual void element_start(XML::Name const& name, HashMap<XML::Name, ByteString> const& attributes) override;
    virtual void element_end(XML::Name const& name) override;
    virtual void text(StringView data) override;
    virtual void comment(StringView data) override;
    virtual void document_end() override;

    JS::NonnullGCPtr<DOM::Document> m_document;
    JS::GCPtr<DOM::Node> m_current_node;
    XMLScriptingSupport m_scripting_support { XMLScriptingSupport::Enabled };
    bool m_has_error { false };
    StringBuilder text_builder;
    Optional<FlyString> m_namespace;

    struct NamespaceStackEntry {
        Optional<FlyString> ns;
        size_t depth;
    };
    Vector<NamespaceStackEntry, 2> m_namespace_stack;
};

}
