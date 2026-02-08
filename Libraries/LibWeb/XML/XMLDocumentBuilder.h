/*
 * Copyright (c) 2022, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Comment.h>
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

Optional<String> resolve_named_html_entity(StringView entity_name);

class XMLDocumentBuilder final : public XML::Listener {
public:
    XMLDocumentBuilder(DOM::Document& document, XMLScriptingSupport = XMLScriptingSupport::Enabled);

    bool has_error() const { return m_has_error; }

private:
    virtual ErrorOr<void> set_source(ByteString) override;
    virtual void set_doctype(XML::Doctype) override;
    virtual void element_start(XML::Name const& name, OrderedHashMap<XML::Name, ByteString> const& attributes) override;
    virtual void element_end(XML::Name const& name) override;
    virtual void text(StringView data) override;
    virtual void comment(StringView data) override;
    virtual void cdata_section(StringView data) override;
    virtual void processing_instruction(StringView target, StringView data) override;
    virtual void document_end() override;

    struct NamespaceAndPrefix {
        FlyString ns;
        Optional<ByteString> prefix;
    };

    Optional<FlyString> namespace_for_name(XML::Name const&);

    GC::Ref<DOM::Document> m_document;
    GC::RootVector<GC::Ref<DOM::Node>> m_template_node_stack;
    GC::Ptr<DOM::Node> m_current_node;
    XMLScriptingSupport m_scripting_support { XMLScriptingSupport::Enabled };
    bool m_has_error { false };
    StringBuilder m_text_builder { StringBuilder::Mode::UTF16 };

    struct NamespaceStackEntry {
        Vector<NamespaceAndPrefix, 2> namespaces;
        size_t depth;
    };
    Vector<NamespaceStackEntry, 2> m_namespace_stack;
};

}
