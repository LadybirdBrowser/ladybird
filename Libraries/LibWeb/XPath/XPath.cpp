/*
 * Copyright (c) 2025, Johannes Gustafsson <johannesgu@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <LibWeb/DOM/Attr.h>
#include <LibWeb/DOM/CDATASection.h>
#include <LibWeb/DOM/Comment.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentFragment.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/NamedNodeMap.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/NodeType.h>
#include <LibWeb/DOM/ProcessingInstruction.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/WebIDL/DOMException.h>

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/valid.h>
#include <libxml/xmlstring.h>
#include <libxml/xpath.h>

#include "XPath.h"

namespace Web::XPath {

static xmlNodePtr mirror_node(xmlDocPtr doc, DOM::Node const& node)
{
    switch (node.type()) {
    case DOM::NodeType::INVALID: {
        return nullptr;
    }
    case DOM::NodeType::ELEMENT_NODE: {
        auto const& element = static_cast<DOM::Element const&>(node);
        ByteString name = element.local_name().bytes_as_string_view();
        auto* xml_element = xmlNewDocNode(doc, nullptr, bit_cast<xmlChar const*>(name.characters()), nullptr);
        xml_element->_private = bit_cast<void*>(&node);
        for (size_t i = 0; i < element.attribute_list_size(); ++i) {
            auto const& attribute = *element.attributes()->item(i);
            ByteString attr_name = attribute.name().bytes_as_string_view();
            ByteString attr_value = attribute.value().bytes_as_string_view();
            auto* attr = xmlSetProp(xml_element, bit_cast<xmlChar const*>(attr_name.characters()), bit_cast<xmlChar const*>(attr_value.characters()));
            attr->_private = bit_cast<void*>(&attribute);

            if (attribute.name() == "id") {
                xmlAddIDSafe(attr, bit_cast<xmlChar const*>(attr_value.characters()));
            }
        }
        auto children = element.children_as_vector();
        for (auto& child : children) {
            xmlAddChild(xml_element, mirror_node(doc, *child));
        }

        return xml_element;
    }
    case DOM::NodeType::ATTRIBUTE_NODE: {
        return nullptr; // Attributes are handled in the elements children above. If this happens, then the attribute is the top node in the document and therefore invalid
    }
    case DOM::NodeType::TEXT_NODE: {
        auto const& text = static_cast<DOM::Text const&>(node);
        auto* xml_text = xmlNewDocText(doc, bit_cast<xmlChar const*>(text.data().to_byte_string().characters()));
        xml_text->_private = bit_cast<void*>(&node);
        return xml_text;
    }
    case DOM::NodeType::CDATA_SECTION_NODE: {
        auto const& cdata = static_cast<DOM::CDATASection const&>(node);
        ByteString data = cdata.data().to_byte_string();
        auto* xml_cdata = xmlNewCDataBlock(doc, bit_cast<xmlChar const*>(data.characters()), data.length());
        xml_cdata->_private = bit_cast<void*>(&node);
        return xml_cdata;
    }
    case DOM::NodeType::ENTITY_REFERENCE_NODE: // Does not seem to be used at all in ladybird
    case DOM::NodeType::ENTITY_NODE:           // Entity nodes are unused in libxml2
    {
        return nullptr;
    }
    case DOM::NodeType::PROCESSING_INSTRUCTION_NODE: {
        auto const& processing_instruction = static_cast<DOM::ProcessingInstruction const&>(node);
        auto* xml_pi = xmlNewDocPI(doc, bit_cast<xmlChar const*>(processing_instruction.target().to_byte_string().characters()), bit_cast<xmlChar const*>(processing_instruction.data().to_byte_string().characters()));
        xml_pi->_private = bit_cast<void*>(&node);
        return xml_pi;
    }
    case DOM::NodeType::COMMENT_NODE: {
        auto const& comment = static_cast<DOM::Comment const&>(node);
        auto* xml_comment = xmlNewDocComment(doc, bit_cast<xmlChar const*>(comment.data().to_byte_string().characters()));
        xml_comment->_private = bit_cast<void*>(&node);
        return xml_comment;
    }
    case DOM::NodeType::DOCUMENT_NODE: {
        auto const& document = static_cast<DOM::Document const&>(node);
        return mirror_node(doc, *document.document_element());
    }
    case DOM::NodeType::DOCUMENT_TYPE_NODE: {
        return nullptr; // Unused in libxml2
    }
    case DOM::NodeType::DOCUMENT_FRAGMENT_NODE: {
        auto const& fragment = static_cast<DOM::DocumentFragment const&>(node);
        auto* xml_fragment = xmlNewDocFragment(doc);
        xml_fragment->_private = bit_cast<void*>(&node);
        auto children = fragment.children_as_vector();
        for (auto& child : children) {
            xmlAddChild(xml_fragment, mirror_node(doc, *child));
        }
        return xml_fragment;
    }
    case DOM::NodeType::NOTATION_NODE: {
        return nullptr; // Unused in libxml2
    }
    }

    return nullptr;
}

static void convert_xpath_result(xmlXPathObjectPtr xpath_result, XPath::XPathResult* result, unsigned short type)
{
    if (!xpath_result) {
        return;
    }

    switch (xpath_result->type) {
    case XPATH_UNDEFINED:
        break;
    case XPATH_NODESET: {
        Vector<GC::Ptr<DOM::Node>> node_list;

        if (xpath_result->nodesetval && xpath_result->nodesetval->nodeNr > 0) {
            node_list.ensure_capacity(xpath_result->nodesetval->nodeNr);
            for (int i = 0; i < xpath_result->nodesetval->nodeNr; i++) {
                auto* node = xpath_result->nodesetval->nodeTab[i];
                auto* dom_node = static_cast<DOM::Node*>(node->_private);

                node_list.unchecked_append(dom_node);
            }
        }

        result->set_node_set(move(node_list), type);
        break;
    }
    case XPATH_BOOLEAN: {
        result->set_boolean(xpath_result->boolval);
        break;
    }
    case XPATH_NUMBER: {
        result->set_number(xpath_result->floatval);
        break;
    }
    case XPATH_STRING: {
        ReadonlyBytes bytes(xpath_result->stringval, xmlStrlen(xpath_result->stringval));
        result->set_string(String::from_utf8_without_validation(bytes));
        break;
    }
    case XPATH_USERS:
    case XPATH_XSLT_TREE: /* An XSLT value tree, non modifiable */
        break;
    }
}

WebIDL::ExceptionOr<GC::Ref<XPathExpression>> create_expression(JS::Realm& realm, String const& expression, GC::Ptr<XPathNSResolver> resolver)
{
    return realm.create<XPathExpression>(realm, expression, resolver);
}

WebIDL::ExceptionOr<GC::Ref<XPathResult>> evaluate(JS::Realm& realm, String const& expression, DOM::Node const& context_node, GC::Ptr<XPathNSResolver> /*resolver*/, unsigned short type, GC::Ptr<XPathResult> result)
{
    // Parse the expression as xpath
    ByteString bytes = expression.bytes_as_string_view();
    auto* xpath_compiled = xmlXPathCompile(bit_cast<xmlChar const*>(bytes.characters()));
    if (!xpath_compiled)
        return WebIDL::SyntaxError::create(realm, "Invalid XPath expression"_utf16);
    ScopeGuard xpath_compiled_cleanup = [&] { xmlXPathFreeCompExpr(xpath_compiled); };

    auto* xml_document = xmlNewDoc(nullptr);
    ScopeGuard xml_cleanup = [&] { xmlFreeDoc(xml_document); };

    if (context_node.type() == DOM::NodeType::DOCUMENT_NODE) {
        xml_document->_private = bit_cast<void*>(&context_node);
    } else {
        xml_document->_private = bit_cast<void*>(&context_node.document());
    }

    auto* xml_node = mirror_node(xml_document, context_node);
    if (!xml_node) {
        return WebIDL::OperationError::create(realm, "XPath evaluation failed"_utf16);
    }

    xmlDocSetRootElement(xml_document, xml_node);

    auto* xpath_context = xmlXPathNewContext(xml_document);
    xmlXPathSetContextNode(xml_node, xpath_context);

    auto* xpath_result = xmlXPathCompiledEval(xpath_compiled, xpath_context);

    ScopeGuard xpath_result_cleanup = [&] {
        xmlXPathFreeObject(xpath_result);
        xmlXPathFreeContext(xpath_context);
    };

    if (!result) {
        result = realm.create<XPathResult>(realm);
    }

    convert_xpath_result(xpath_result, result, type);

    return GC::Ref<XPathResult>(*result);
}

}
