/*
 * Copyright (c) 2022, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <AK/Utf16StringBuilder.h>
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
    virtual void element_start(Utf16FlyString const& name, Vector<XML::ListenerAttribute> const& attributes) override;
    virtual void element_end(Utf16FlyString const& name) override;
    virtual void text(StringView data) override;
    virtual void comment(StringView data) override;
    virtual void cdata_section(StringView data) override;
    virtual void processing_instruction(Utf16FlyString const& target, Utf16String const& data) override;
    virtual void document_end() override;

    struct NamespaceAndPrefix {
        Utf16FlyString ns;
        Optional<Utf16FlyString> prefix;
    };

    Optional<Utf16FlyString> namespace_for_name(Utf16FlyString const&);

    GC::Ref<DOM::Document> m_document;
    GC::RootVector<GC::Ref<DOM::Node>> m_template_node_stack;
    GC::Ptr<DOM::Node> m_current_node;
    XMLScriptingSupport m_scripting_support { XMLScriptingSupport::Enabled };
    bool m_has_error { false };
    Utf16StringBuilder m_text_builder;

    struct NamespaceStackEntry {
        Vector<NamespaceAndPrefix, 2> namespaces;
        size_t depth;
    };
    Vector<NamespaceStackEntry, 2> m_namespace_stack;
};

}
