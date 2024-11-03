/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/XMLSerializerPrototype.h>
#include <LibWeb/DOM/Attr.h>
#include <LibWeb/DOM/CDATASection.h>
#include <LibWeb/DOM/Comment.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentFragment.h>
#include <LibWeb/DOM/DocumentType.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/NamedNodeMap.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/ProcessingInstruction.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/DOMParsing/XMLSerializer.h>
#include <LibWeb/HTML/HTMLTemplateElement.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::DOMParsing {

JS_DEFINE_ALLOCATOR(XMLSerializer);

WebIDL::ExceptionOr<JS::NonnullGCPtr<XMLSerializer>> XMLSerializer::construct_impl(JS::Realm& realm)
{
    return realm.heap().allocate<XMLSerializer>(realm, realm);
}

XMLSerializer::XMLSerializer(JS::Realm& realm)
    : PlatformObject(realm)
{
}

XMLSerializer::~XMLSerializer() = default;

void XMLSerializer::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(XMLSerializer);
}

// https://w3c.github.io/DOM-Parsing/#dom-xmlserializer-serializetostring
WebIDL::ExceptionOr<String> XMLSerializer::serialize_to_string(JS::NonnullGCPtr<DOM::Node const> root)
{
    // The serializeToString(root) method must produce an XML serialization of root passing a value of false for the require well-formed parameter, and return the result.
    return serialize_node_to_xml_string(root, RequireWellFormed::No);
}

// https://w3c.github.io/DOM-Parsing/#dfn-add
static void add_prefix_to_namespace_prefix_map(HashMap<FlyString, Vector<Optional<FlyString>>>& prefix_map, Optional<FlyString> const& prefix, Optional<FlyString> const& namespace_)
{
    // 1. Let candidates list be the result of retrieving a list from map where there exists a key in map that matches the value of ns or if there is no such key, then let candidates list be null.
    auto candidates_list_iterator = namespace_.has_value() ? prefix_map.find(*namespace_) : prefix_map.end();

    // 2. If candidates list is null, then create a new list with prefix as the only item in the list, and associate that list with a new key ns in map.
    if (candidates_list_iterator == prefix_map.end()) {
        Vector<Optional<FlyString>> new_list;
        new_list.append(prefix);
        prefix_map.set(*namespace_, move(new_list));
        return;
    }

    // 3. Otherwise, append prefix to the end of candidates list.
    candidates_list_iterator->value.append(prefix);
}

// https://w3c.github.io/DOM-Parsing/#dfn-retrieving-a-preferred-prefix-string
static Optional<FlyString> retrieve_a_preferred_prefix_string(Optional<FlyString> const& preferred_prefix, HashMap<FlyString, Vector<Optional<FlyString>>> const& namespace_prefix_map, Optional<FlyString> const& namespace_)
{
    // 1. Let candidates list be the result of retrieving a list from map where there exists a key in map that matches the value of ns or if there is no such key,
    //    then stop running these steps, and return the null value.
    if (!namespace_.has_value())
        return {};
    auto candidates_list_iterator = namespace_prefix_map.find(*namespace_);
    if (candidates_list_iterator == namespace_prefix_map.end())
        return {};

    // 2. Otherwise, for each prefix value prefix in candidates list, iterating from beginning to end:
    for (size_t prefix_index = 0; prefix_index < candidates_list_iterator->value.size(); ++prefix_index) {
        auto const& prefix = candidates_list_iterator->value.at(prefix_index);

        // 1. If prefix matches preferred prefix, then stop running these steps and return prefix.
        if (prefix == preferred_prefix)
            return prefix;

        // 2. If prefix is the last item in the candidates list, then stop running these steps and return prefix.
        if (prefix_index == candidates_list_iterator->value.size() - 1)
            return prefix;
    }

    // Spec Note: There will always be at least one prefix value in the list.
    VERIFY_NOT_REACHED();
}

// https://w3c.github.io/DOM-Parsing/#dfn-generating-a-prefix
static FlyString generate_a_prefix(HashMap<FlyString, Vector<Optional<FlyString>>>& namespace_prefix_map, Optional<FlyString> const& new_namespace, u64& prefix_index)
{
    // 1. Let generated prefix be the concatenation of the string "ns" and the current numerical value of prefix index.
    auto generated_prefix = FlyString(MUST(String::formatted("ns{}", prefix_index)));

    // 2. Let the value of prefix index be incremented by one.
    ++prefix_index;

    // 3. Add to map the generated prefix given the new namespace namespace.
    add_prefix_to_namespace_prefix_map(namespace_prefix_map, generated_prefix, new_namespace);

    // 4. Return the value of generated prefix.
    return generated_prefix;
}

// https://w3c.github.io/DOM-Parsing/#dfn-found
static bool prefix_is_in_prefix_map(FlyString const& prefix, HashMap<FlyString, Vector<Optional<FlyString>>> const& namespace_prefix_map, Optional<FlyString> const& namespace_)
{
    // 1. Let candidates list be the result of retrieving a list from map where there exists a key in map that matches the value of ns
    //    or if there is no such key, then stop running these steps, and return false.
    if (!namespace_.has_value())
        return false;
    auto candidates_list_iterator = namespace_prefix_map.find(*namespace_);
    if (candidates_list_iterator == namespace_prefix_map.end())
        return false;

    // 2. If the value of prefix occurs at least once in candidates list, return true, otherwise return false.
    return candidates_list_iterator->value.contains_slow(prefix);
}

WebIDL::ExceptionOr<String> serialize_node_to_xml_string_impl(JS::NonnullGCPtr<DOM::Node const> root, Optional<FlyString>& namespace_, HashMap<FlyString, Vector<Optional<FlyString>>>& namespace_prefix_map, u64& prefix_index, RequireWellFormed require_well_formed);

// https://w3c.github.io/DOM-Parsing/#dfn-xml-serialization
WebIDL::ExceptionOr<String> serialize_node_to_xml_string(JS::NonnullGCPtr<DOM::Node const> root, RequireWellFormed require_well_formed)
{
    // 1. Let namespace be a context namespace with value null. The context namespace tracks the XML serialization algorithm's current default namespace.
    //    The context namespace is changed when either an Element Node has a default namespace declaration, or the algorithm generates a default namespace declaration
    //    for the Element Node to match its own namespace. The algorithm assumes no namespace (null) to start.
    Optional<FlyString> namespace_;

    // 2. Let prefix map be a new namespace prefix map.
    HashMap<FlyString, Vector<Optional<FlyString>>> prefix_map;

    // 3. Add the XML namespace with prefix value "xml" to prefix map.
    add_prefix_to_namespace_prefix_map(prefix_map, "xml"_fly_string, Namespace::XML);

    // 4. Let prefix index be a generated namespace prefix index with value 1. The generated namespace prefix index is used to generate a new unique prefix value
    //    when no suitable existing namespace prefix is available to serialize a node's namespaceURI (or the namespaceURI of one of node's attributes).
    u64 prefix_index = 1;

    // 5. Return the result of running the XML serialization algorithm on node passing the context namespace namespace, namespace prefix map prefix map,
    //    generated namespace prefix index reference to prefix index, and the flag require well-formed. If an exception occurs during the execution of the algorithm,
    //    then catch that exception and throw an "InvalidStateError" DOMException.
    // NOTE: InvalidStateError exceptions will be created when needed, as this also allows us to have a specific error message for the exception.
    return serialize_node_to_xml_string_impl(root, namespace_, prefix_map, prefix_index, require_well_formed);
}

static WebIDL::ExceptionOr<String> serialize_element(DOM::Element const& element, Optional<FlyString>& namespace_, HashMap<FlyString, Vector<Optional<FlyString>>>& namespace_prefix_map, u64& prefix_index, RequireWellFormed require_well_formed);
static WebIDL::ExceptionOr<String> serialize_document(DOM::Document const& document, Optional<FlyString>& namespace_, HashMap<FlyString, Vector<Optional<FlyString>>>& namespace_prefix_map, u64& prefix_index, RequireWellFormed require_well_formed);
static WebIDL::ExceptionOr<String> serialize_comment(DOM::Comment const& comment, RequireWellFormed require_well_formed);
static WebIDL::ExceptionOr<String> serialize_text(DOM::Text const& text, RequireWellFormed require_well_formed);
static WebIDL::ExceptionOr<String> serialize_document_fragment(DOM::DocumentFragment const& document_fragment, Optional<FlyString>& namespace_, HashMap<FlyString, Vector<Optional<FlyString>>>& namespace_prefix_map, u64& prefix_index, RequireWellFormed require_well_formed);
static WebIDL::ExceptionOr<String> serialize_document_type(DOM::DocumentType const& document_type, RequireWellFormed require_well_formed);
static WebIDL::ExceptionOr<String> serialize_processing_instruction(DOM::ProcessingInstruction const& processing_instruction, RequireWellFormed require_well_formed);
static WebIDL::ExceptionOr<String> serialize_cdata_section(DOM::CDATASection const& cdata_section, RequireWellFormed require_well_formed);

// https://w3c.github.io/DOM-Parsing/#dfn-xml-serialization-algorithm
WebIDL::ExceptionOr<String> serialize_node_to_xml_string_impl(JS::NonnullGCPtr<DOM::Node const> root, Optional<FlyString>& namespace_, HashMap<FlyString, Vector<Optional<FlyString>>>& namespace_prefix_map, u64& prefix_index, RequireWellFormed require_well_formed)
{
    // Each of the following algorithms for producing an XML serialization of a DOM node take as input a node to serialize and the following arguments:
    // - A context namespace namespace
    // - A namespace prefix map prefix map
    // - A generated namespace prefix index prefix index
    // - The require well-formed flag

    // The XML serialization algorithm produces an XML serialization of an arbitrary DOM node node based on the node's interface type.
    // Each referenced algorithm is to be passed the arguments as they were received by the caller and return their result to the caller.
    // Re-throw any exceptions.
    // If node's interface is:

    if (is<DOM::Element>(*root)) {
        // -> Element
        //    Run the algorithm for XML serializing an Element node node.
        return serialize_element(static_cast<DOM::Element const&>(*root), namespace_, namespace_prefix_map, prefix_index, require_well_formed);
    }

    if (is<DOM::Document>(*root)) {
        // -> Document
        //    Run the algorithm for XML serializing a Document node node.
        return serialize_document(static_cast<DOM::Document const&>(*root), namespace_, namespace_prefix_map, prefix_index, require_well_formed);
    }

    if (is<DOM::Comment>(*root)) {
        // -> Comment
        //    Run the algorithm for XML serializing a Comment node node.
        return serialize_comment(static_cast<DOM::Comment const&>(*root), require_well_formed);
    }

    if (is<DOM::Text>(*root)) {
        // -> Text
        //    Run the algorithm for XML serializing a Text node node.
        return serialize_text(static_cast<DOM::Text const&>(*root), require_well_formed);
    }

    if (is<DOM::DocumentFragment>(*root)) {
        // -> DocumentFragment
        //    Run the algorithm for XML serializing a DocumentFragment node node.
        return serialize_document_fragment(static_cast<DOM::DocumentFragment const&>(*root), namespace_, namespace_prefix_map, prefix_index, require_well_formed);
    }

    if (is<DOM::DocumentType>(*root)) {
        // -> DocumentType
        //    Run the algorithm for XML serializing a DocumentType node node.
        return serialize_document_type(static_cast<DOM::DocumentType const&>(*root), require_well_formed);
    }

    if (is<DOM::ProcessingInstruction>(*root)) {
        // -> ProcessingInstruction
        //    Run the algorithm for XML serializing a ProcessingInstruction node node.
        return serialize_processing_instruction(static_cast<DOM::ProcessingInstruction const&>(*root), require_well_formed);
    }

    if (is<DOM::CDATASection>(*root)) {
        // Note: Serialization of CDATASection nodes is not mentioned in the specification, but treating CDATASection nodes as
        // text leads to incorrect serialization.
        return serialize_cdata_section(static_cast<DOM::CDATASection const&>(*root), require_well_formed);
    }

    if (is<DOM::Attr>(*root)) {
        // -> An Attr object
        //    Return an empty string.
        return String {};
    }

    // -> Anything else
    //    Throw a TypeError. Only Nodes and Attr objects can be serialized by this algorithm.
    return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Can only serialize Nodes or Attributes."sv };
}

// https://w3c.github.io/DOM-Parsing/#dfn-recording-the-namespace-information
static Optional<FlyString> record_namespace_information(DOM::Element const& element, HashMap<FlyString, Vector<Optional<FlyString>>>& namespace_prefix_map, HashMap<FlyString, Optional<FlyString>>& local_prefix_map)
{
    // 1. Let default namespace attr value be null.
    Optional<FlyString> default_namespace_attribute_value;

    // 2. Main: For each attribute attr in element's attributes, in the order they are specified in the element's attribute list:
    for (size_t attribute_index = 0; attribute_index < element.attributes()->length(); ++attribute_index) {
        auto const* attribute = element.attributes()->item(attribute_index);
        VERIFY(attribute);

        // 1. Let attribute namespace be the value of attr's namespaceURI value.
        auto const& attribute_namespace = attribute->namespace_uri();

        // 2. Let attribute prefix be the value of attr's prefix.
        auto const& attribute_prefix = attribute->prefix();

        // 3. If the attribute namespace is the XMLNS namespace, then:
        if (attribute_namespace == Namespace::XMLNS) {
            // 1. If attribute prefix is null, then attr is a default namespace declaration. Set the default namespace attr value to attr's value and stop running these steps,
            //    returning to Main to visit the next attribute.
            if (!attribute_prefix.has_value()) {
                default_namespace_attribute_value = attribute->value();
                continue;
            }

            // 2. Otherwise, the attribute prefix is not null and attr is a namespace prefix definition. Run the following steps:
            // 1. Let prefix definition be the value of attr's localName.
            auto const& prefix_definition = attribute->local_name();

            // 2. Let namespace definition be the value of attr's value.
            Optional<FlyString> namespace_definition = attribute->value();

            // 3. If namespace definition is the XML namespace, then stop running these steps, and return to Main to visit the next attribute.
            if (namespace_definition == Namespace::XML)
                continue;

            // 4. If namespace definition is the empty string (the declarative form of having no namespace), then let namespace definition be null instead.
            if (namespace_definition == ""sv)
                namespace_definition = {};

            // 5. If prefix definition is found in map given the namespace namespace definition, then stop running these steps, and return to Main to visit the next attribute.
            if (prefix_is_in_prefix_map(prefix_definition, namespace_prefix_map, namespace_definition))
                continue;

            // 6. Add the prefix prefix definition to map given namespace namespace definition.
            add_prefix_to_namespace_prefix_map(namespace_prefix_map, prefix_definition, namespace_definition);

            // 7. Add the value of prefix definition as a new key to the local prefixes map, with the namespace definition as the key's value replacing the value of null with the empty string if applicable.
            local_prefix_map.set(prefix_definition, namespace_definition.value_or(FlyString {}));
        }
    }

    // 3. Return the value of default namespace attr value.
    return default_namespace_attribute_value;
}

// https://w3c.github.io/DOM-Parsing/#dfn-serializing-an-attribute-value
static WebIDL::ExceptionOr<String> serialize_an_attribute_value(Optional<FlyString> const& attribute_value, [[maybe_unused]] RequireWellFormed require_well_formed)
{
    // FIXME: 1. If the require well-formed flag is set (its value is true), and attribute value contains characters that are not matched by the XML Char production,
    //           then throw an exception; the serialization of this attribute value would fail to produce a well-formed element serialization.

    // 2. If attribute value is null, then return the empty string.
    if (!attribute_value.has_value())
        return String {};

    // 3. Otherwise, attribute value is a string. Return the value of attribute value, first replacing any occurrences of the following:
    auto final_attribute_value = attribute_value->to_string();

    // 1. "&" with "&amp;"
    final_attribute_value = MUST(final_attribute_value.replace("&"sv, "&amp;"sv, ReplaceMode::All));

    // 2. """ with "&quot;"
    final_attribute_value = MUST(final_attribute_value.replace("\""sv, "&quot;"sv, ReplaceMode::All));

    // 3. "<" with "&lt;"
    final_attribute_value = MUST(final_attribute_value.replace("<"sv, "&lt;"sv, ReplaceMode::All));

    // 4. ">" with "&gt;"
    final_attribute_value = MUST(final_attribute_value.replace(">"sv, "&gt;"sv, ReplaceMode::All));

    return final_attribute_value;
}

struct LocalNameSetEntry {
    Optional<FlyString> namespace_uri;
    FlyString local_name;
};

// https://w3c.github.io/DOM-Parsing/#dfn-xml-serialization-of-the-attributes
static WebIDL::ExceptionOr<String> serialize_element_attributes(DOM::Element const& element, HashMap<FlyString, Vector<Optional<FlyString>>>& namespace_prefix_map, u64& prefix_index, HashMap<FlyString, Optional<FlyString>> const& local_prefixes_map, bool ignore_namespace_definition_attribute, RequireWellFormed require_well_formed)
{
    auto& realm = element.realm();

    // 1. Let result be the empty string.
    StringBuilder result;

    // 2. Let localname set be a new empty namespace localname set. This localname set will contain tuples of unique attribute namespaceURI and localName pairs, and is populated as each attr is processed.
    // Spec Note: This set is used to [optionally] enforce the well-formed constraint that an element cannot have two attributes with the same namespaceURI and localName.
    //            This can occur when two otherwise identical attributes on the same element differ only by their prefix values.
    Vector<LocalNameSetEntry> local_name_set;

    // 3. Loop: For each attribute attr in element's attributes, in the order they are specified in the element's attribute list:
    for (size_t attribute_index = 0; attribute_index < element.attributes()->length(); ++attribute_index) {
        auto const* attribute = element.attributes()->item(attribute_index);
        VERIFY(attribute);

        // 1. If the require well-formed flag is set (its value is true), and the localname set contains a tuple whose values match those of a new tuple consisting of attr's namespaceURI attribute and localName attribute,
        //      then throw an exception; the serialization of this attr would fail to produce a well-formed element serialization.
        if (require_well_formed == RequireWellFormed::Yes) {
            auto local_name_set_iterator = local_name_set.find_if([&attribute](LocalNameSetEntry const& entry) {
                return entry.namespace_uri == attribute->namespace_uri() && entry.local_name == attribute->local_name();
            });

            if (local_name_set_iterator != local_name_set.end())
                return WebIDL::InvalidStateError::create(realm, "Element contains two attributes with identical namespaces and local names"_string);
        }

        // 2. Create a new tuple consisting of attr's namespaceURI attribute and localName attribute, and add it to the localname set.
        LocalNameSetEntry new_local_name_set_entry {
            .namespace_uri = attribute->namespace_uri(),
            .local_name = attribute->local_name(),
        };

        local_name_set.append(move(new_local_name_set_entry));

        // 3. Let attribute namespace be the value of attr's namespaceURI value.
        auto const& attribute_namespace = attribute->namespace_uri();

        // 4. Let candidate prefix be null.
        Optional<FlyString> candidate_prefix;

        // 5. If attribute namespace is not null, then run these sub-steps:
        if (attribute_namespace.has_value()) {
            // 1. Let candidate prefix be the result of retrieving a preferred prefix string from map given namespace attribute namespace with preferred prefix being attr's prefix value.
            candidate_prefix = retrieve_a_preferred_prefix_string(attribute->prefix(), namespace_prefix_map, attribute->namespace_uri());

            // 2. If the value of attribute namespace is the XMLNS namespace, then run these steps:
            if (attribute_namespace == Namespace::XMLNS) {
                // 1. If any of the following are true, then stop running these steps and goto Loop to visit the next attribute:
                // - the attr's value is the XML namespace;
                if (attribute->value() == Namespace::XML)
                    continue;

                // - the attr's prefix is null and the ignore namespace definition attribute flag is true (the Element's default namespace attribute should be skipped);
                if (!attribute->prefix().has_value() && ignore_namespace_definition_attribute)
                    continue;

                // - the attr's prefix is not null and either
                if (attribute->prefix().has_value()) {
                    // - the attr's localName is not a key contained in the local prefixes map, or
                    auto name_in_local_prefix_map_iterator = local_prefixes_map.find(attribute->local_name());
                    if (name_in_local_prefix_map_iterator == local_prefixes_map.end())
                        continue;

                    // - the attr's localName is present in the local prefixes map but the value of the key does not match attr's value
                    if (name_in_local_prefix_map_iterator->value != attribute->value())
                        continue;
                }

                // and furthermore that the attr's localName (as the prefix to find) is found in the namespace prefix map given the namespace consisting of the attr's value
                // (the current namespace prefix definition was exactly defined previously--on an ancestor element not the current element whose attributes are being processed).
                if (prefix_is_in_prefix_map(attribute->local_name(), namespace_prefix_map, attribute->value()))
                    continue;

                // 2. If the require well-formed flag is set (its value is true), and the value of attr's value attribute matches the XMLNS namespace,
                //    then throw an exception; the serialization of this attribute would produce invalid XML because the XMLNS namespace is reserved and cannot be applied as an element's namespace via XML parsing.
                if (require_well_formed == RequireWellFormed::Yes && attribute->value() == Namespace::XMLNS)
                    return WebIDL::InvalidStateError::create(realm, "The XMLNS namespace cannot be used as an element's namespace"_string);

                // 3. If the require well-formed flag is set (its value is true), and the value of attr's value attribute is the empty string,
                //    then throw an exception; namespace prefix declarations cannot be used to undeclare a namespace (use a default namespace declaration instead).
                if (require_well_formed == RequireWellFormed::Yes && attribute->value().is_empty())
                    return WebIDL::InvalidStateError::create(realm, "Attribute's value is empty"_string);

                // 4. [If] the attr's prefix matches the string "xmlns", then let candidate prefix be the string "xmlns".
                if (attribute->prefix() == "xmlns"sv)
                    candidate_prefix = "xmlns"_fly_string;
            }

            // 3. Otherwise, the attribute namespace in not the XMLNS namespace. Run these steps:
            else {
                // 1. Let candidate prefix be the result of generating a prefix providing map, attribute namespace, and prefix index as input.
                candidate_prefix = generate_a_prefix(namespace_prefix_map, attribute->namespace_uri(), prefix_index);

                // 2. Append the following to result, in the order listed:
                // 1. " " (U+0020 SPACE);
                // 2. The string "xmlns:";
                result.append(" xmlns:"sv);

                // 3. The value of candidate prefix;
                VERIFY(candidate_prefix.has_value());
                result.append(candidate_prefix.value());

                // 4. "="" (U+003D EQUALS SIGN, U+0022 QUOTATION MARK);
                result.append("=\""sv);

                // 5. The result of serializing an attribute value given attribute namespace and the require well-formed flag as input
                result.append(TRY(serialize_an_attribute_value(attribute->namespace_uri(), require_well_formed)));

                // 6. """ (U+0022 QUOTATION MARK).
                result.append('"');
            }
        }

        // 6. Append a " " (U+0020 SPACE) to result.
        result.append(' ');

        // 7. If candidate prefix is not null, then append to result the concatenation of candidate prefix with ":" (U+003A COLON).
        if (candidate_prefix.has_value())
            result.appendff("{}:", candidate_prefix.value());

        // 8. If the require well-formed flag is set (its value is true), and this attr's localName attribute contains the character ":" (U+003A COLON)
        //    or does not match the XML Name production or equals "xmlns" and attribute namespace is null, then throw an exception; the serialization of this attr would not be a well-formed attribute.
        if (require_well_formed == RequireWellFormed::Yes) {
            if (attribute->local_name().bytes_as_string_view().contains(':'))
                return WebIDL::InvalidStateError::create(realm, "Attribute's local name contains a colon"_string);

            // FIXME: Check attribute's local name against the XML Name production.

            if (attribute->local_name() == "xmlns"sv && !attribute->namespace_uri().has_value())
                return WebIDL::InvalidStateError::create(realm, "Attribute's local name is 'xmlns' and the attribute has no namespace"_string);
        }

        // 9. Append the following strings to result, in the order listed:
        // 1. The value of attr's localName;
        result.append(attribute->local_name());

        // 2. "="" (U+003D EQUALS SIGN, U+0022 QUOTATION MARK);
        result.append("=\""sv);

        // 3. The result of serializing an attribute value given attr's value attribute and the require well-formed flag as input;
        result.append(TRY(serialize_an_attribute_value(attribute->value(), require_well_formed)));

        // 4. """ (U+0022 QUOTATION MARK).
        result.append('"');
    }

    // 4. Return the value of result.
    return MUST(result.to_string());
}

// https://w3c.github.io/DOM-Parsing/#xml-serializing-an-element-node
static WebIDL::ExceptionOr<String> serialize_element(DOM::Element const& element, Optional<FlyString>& namespace_, HashMap<FlyString, Vector<Optional<FlyString>>>& namespace_prefix_map, u64& prefix_index, RequireWellFormed require_well_formed)
{
    auto& realm = element.realm();

    // 1. If the require well-formed flag is set (its value is true), and this node's localName attribute contains the character ":" (U+003A COLON) or does not match the XML Name production,
    //    then throw an exception; the serialization of this node would not be a well-formed element.
    if (require_well_formed == RequireWellFormed::Yes) {
        if (element.local_name().bytes_as_string_view().contains(':'))
            return WebIDL::InvalidStateError::create(realm, "Element's local name contains a colon"_string);

        // FIXME: Check element's local name against the XML Char production.
    }

    // 2. Let markup be the string "<" (U+003C LESS-THAN SIGN).
    StringBuilder markup;
    markup.append('<');

    // 3. Let qualified name be an empty string.
    StringBuilder qualified_name;

    // 4. Let skip end tag be a boolean flag with value false.
    bool skip_end_tag = false;

    // 5. Let ignore namespace definition attribute be a boolean flag with value false.
    bool ignore_namespace_definition_attribute = false;

    // 6. Given prefix map, copy a namespace prefix map and let map be the result.
    HashMap<FlyString, Vector<Optional<FlyString>>> map;

    // https://w3c.github.io/DOM-Parsing/#dfn-copy-a-namespace-prefix-map
    // NOTE: This is only used here.
    // To copy a namespace prefix map map means to copy the map's keys into a new empty namespace prefix map,
    // and to copy each of the values in the namespace prefix list associated with each keys' value into a new list
    // which should be associated with the respective key in the new map.
    for (auto const& map_entry : namespace_prefix_map)
        map.set(map_entry.key, map_entry.value);

    // 7. Let local prefixes map be an empty map. The map has unique Node prefix strings as its keys, with corresponding namespaceURI Node values
    //    as the map's key values (in this map, the null namespace is represented by the empty string).
    HashMap<FlyString, Optional<FlyString>> local_prefixes_map;

    // 8. Let local default namespace be the result of recording the namespace information for node given map and local prefixes map.
    auto local_default_namespace = record_namespace_information(element, map, local_prefixes_map);

    // 9. Let inherited ns be a copy of namespace.
    auto inherited_ns = namespace_;

    // 10. Let ns be the value of node's namespaceURI attribute.
    auto const& ns = element.namespace_uri();

    // 11. If inherited ns is equal to ns, then:
    if (inherited_ns == ns) {
        // 1. If local default namespace is not null, then set ignore namespace definition attribute to true.
        if (local_default_namespace.has_value())
            ignore_namespace_definition_attribute = true;

        // 2. If ns is the XML namespace, then append to qualified name the concatenation of the string "xml:" and the value of node's localName.
        if (ns == Namespace::XML)
            qualified_name.appendff("xml:{}", element.local_name());

        // 3. Otherwise, append to qualified name the value of node's localName.
        else
            qualified_name.append(element.local_name());

        // 4. Append the value of qualified name to markup.
        markup.append(qualified_name.string_view());
    }

    // 12. Otherwise, inherited ns is not equal to ns (the node's own namespace is different from the context namespace of its parent). Run these sub-steps:
    else {
        // 1. Let prefix be the value of node's prefix attribute.
        auto prefix = element.prefix();

        // 2. Let candidate prefix be the result of retrieving a preferred prefix string prefix from map given namespace ns.
        auto candidate_prefix = retrieve_a_preferred_prefix_string(prefix, map, ns);

        // 3. If the value of prefix matches "xmlns", then run the following steps:
        if (prefix == "xmlns"sv) {
            // 1. If the require well-formed flag is set, then throw an error. An Element with prefix "xmlns" will not legally round-trip in a conforming XML parser.
            if (require_well_formed == RequireWellFormed::Yes)
                return WebIDL::InvalidStateError::create(realm, "Elements prefix is 'xmlns'"_string);

            // 2. Let candidate prefix be the value of prefix.
            candidate_prefix = prefix;
        }

        // 4. Found a suitable namespace prefix: if candidate prefix is not null (a namespace prefix is defined which maps to ns), then:
        if (candidate_prefix.has_value()) {
            // 1. Append to qualified name the concatenation of candidate prefix, ":" (U+003A COLON), and node's localName.
            qualified_name.appendff("{}:{}", candidate_prefix.value(), element.local_name());

            // 2. If the local default namespace is not null (there exists a locally-defined default namespace declaration attribute) and its value is not the XML namespace,
            //   then let inherited ns get the value of local default namespace unless the local default namespace is the empty string in which case let it get null
            //   (the context namespace is changed to the declared default, rather than this node's own namespace).
            if (local_default_namespace.has_value() && local_default_namespace.value() != Namespace::XML) {
                if (!local_default_namespace.value().is_empty())
                    inherited_ns = local_default_namespace.value();
                else
                    inherited_ns = {};
            }

            // 3. Append the value of qualified name to markup.
            markup.append(qualified_name.string_view());
        }

        // 5. Otherwise, if prefix is not null, then:
        else if (prefix.has_value()) {
            // 1. If the local prefixes map contains a key matching prefix, then let prefix be the result of generating a prefix providing as input map, ns, and prefix index.
            if (local_prefixes_map.contains(*prefix))
                prefix = generate_a_prefix(map, ns, prefix_index);

            // 2. Add prefix to map given namespace ns.
            add_prefix_to_namespace_prefix_map(map, prefix, ns);

            // 3. Append to qualified name the concatenation of prefix, ":" (U+003A COLON), and node's localName.
            qualified_name.appendff("{}:{}", prefix, element.local_name());

            // 4. Append the value of qualified name to markup.
            markup.append(qualified_name.string_view());

            // 5. Append the following to markup, in the order listed:
            // 1. " " (U+0020 SPACE);
            // 2. The string "xmlns:";
            markup.append(" xmlns:"sv);

            // 3. The value of prefix;
            markup.append(prefix.value());

            // 4. "="" (U+003D EQUALS SIGN, U+0022 QUOTATION MARK);
            markup.append("=\""sv);

            // 5. The result of serializing an attribute value given ns and the require well-formed flag as input;
            markup.append(TRY(serialize_an_attribute_value(ns, require_well_formed)));

            // 6. """ (U+0022 QUOTATION MARK).
            markup.append('"');

            // 7. If local default namespace is not null (there exists a locally-defined default namespace declaration attribute),
            //   then let inherited ns get the value of local default namespace unless the local default namespace is the empty string in which case let it get null.
            if (local_default_namespace.has_value()) {
                if (!local_default_namespace.value().is_empty())
                    inherited_ns = local_default_namespace.value();
                else
                    inherited_ns = {};
            }
        }

        // 6. Otherwise, if local default namespace is null, or local default namespace is not null and its value is not equal to ns, then:
        else if (!local_default_namespace.has_value() || local_default_namespace.value() != ns) {
            // 1. Set the ignore namespace definition attribute flag to true.
            ignore_namespace_definition_attribute = true;

            // 2. Append to qualified name the value of node's localName.
            qualified_name.append(element.local_name());

            // 3. Let the value of inherited ns be ns.
            inherited_ns = ns;

            // 4. Append the value of qualified name to markup.
            markup.append(qualified_name.string_view());

            // 5. Append the following to markup, in the order listed:
            // 1. " " (U+0020 SPACE);
            // 2. The string "xmlns";
            // 3. "="" (U+003D EQUALS SIGN, U+0022 QUOTATION MARK);
            markup.append(" xmlns=\""sv);

            // 4. The result of serializing an attribute value given ns and the require well-formed flag as input;
            markup.append(TRY(serialize_an_attribute_value(ns, require_well_formed)));

            // 5. """ (U+0022 QUOTATION MARK).
            markup.append('"');
        }

        else {
            // 7. Otherwise, the node has a local default namespace that matches ns.
            //    Append to qualified name the value of node's localName, let the value of inherited ns be ns, and append the value of qualified name to markup.
            VERIFY(local_default_namespace.has_value());
            VERIFY(local_default_namespace.value() == ns);

            qualified_name.append(element.local_name());
            inherited_ns = ns;
            markup.append(qualified_name.string_view());
        }
    }

    // 13. Append to markup the result of the XML serialization of node's attributes given map, prefix index, local prefixes map, ignore namespace definition attribute flag, and require well-formed flag.
    markup.append(TRY(serialize_element_attributes(element, map, prefix_index, local_prefixes_map, ignore_namespace_definition_attribute, require_well_formed)));

    // 14. If ns is the HTML namespace, and the node's list of children is empty, and the node's localName matches any one of the following void elements:
    //    "area", "base", "basefont", "bgsound", "br", "col", "embed", "frame", "hr", "img", "input", "keygen", "link", "menuitem", "meta", "param", "source", "track", "wbr";
    //    then append the following to markup, in the order listed:
    if (ns == Namespace::HTML && !element.has_children() && element.local_name().is_one_of(HTML::TagNames::area, HTML::TagNames::area, HTML::TagNames::base, HTML::TagNames::basefont, HTML::TagNames::bgsound, HTML::TagNames::br, HTML::TagNames::col, HTML::TagNames::embed, HTML::TagNames::frame, HTML::TagNames::hr, HTML::TagNames::img, HTML::TagNames::input, HTML::TagNames::keygen, HTML::TagNames::link, HTML::TagNames::menuitem, HTML::TagNames::meta, HTML::TagNames::param, HTML::TagNames::source, HTML::TagNames::track, HTML::TagNames::wbr)) {
        // 1. " " (U+0020 SPACE);
        // 2. "/" (U+002F SOLIDUS).
        markup.append(" /"sv);

        // and set the skip end tag flag to true.
        skip_end_tag = true;
    }

    // 15. If ns is not the HTML namespace, and the node's list of children is empty, then append "/" (U+002F SOLIDUS) to markup and set the skip end tag flag to true.
    if (ns != Namespace::HTML && !element.has_children()) {
        markup.append('/');
        skip_end_tag = true;
    }

    // 16. Append ">" (U+003E GREATER-THAN SIGN) to markup.
    markup.append('>');

    // 17. If the value of skip end tag is true, then return the value of markup and skip the remaining steps. The node is a leaf-node.
    if (skip_end_tag)
        return MUST(markup.to_string());

    // 18. If ns is the HTML namespace, and the node's localName matches the string "template", then this is a template element.
    if (ns == Namespace::HTML && element.local_name() == HTML::TagNames::template_) {
        // Append to markup the result of XML serializing a DocumentFragment node given the template element's template contents (a DocumentFragment), providing inherited ns, map, prefix index, and the require well-formed flag.
        auto const& template_element = verify_cast<HTML::HTMLTemplateElement>(element);
        markup.append(TRY(serialize_document_fragment(template_element.content(), inherited_ns, map, prefix_index, require_well_formed)));
    }

    // 19. Otherwise, append to markup the result of running the XML serialization algorithm on each of node's children, in tree order, providing inherited ns, map, prefix index, and the require well-formed flag.
    else {
        for (auto const* element_child = element.first_child(); element_child; element_child = element_child->next_sibling())
            markup.append(TRY(serialize_node_to_xml_string_impl(*element_child, inherited_ns, map, prefix_index, require_well_formed)));
    }

    // 20. Append the following to markup, in the order listed:
    // 1. "</" (U+003C LESS-THAN SIGN, U+002F SOLIDUS);
    markup.append("</"sv);

    // 2. The value of qualified name;
    markup.append(qualified_name.string_view());

    // 3. ">" (U+003E GREATER-THAN SIGN).
    markup.append('>');

    // 21. Return the value of markup.
    return MUST(markup.to_string());
}

// https://w3c.github.io/DOM-Parsing/#xml-serializing-a-document-node
static WebIDL::ExceptionOr<String> serialize_document(DOM::Document const& document, Optional<FlyString>& namespace_, HashMap<FlyString, Vector<Optional<FlyString>>>& namespace_prefix_map, u64& prefix_index, RequireWellFormed require_well_formed)
{
    // If the require well-formed flag is set (its value is true), and this node has no documentElement (the documentElement attribute's value is null),
    // then throw an exception; the serialization of this node would not be a well-formed document.
    if (require_well_formed == RequireWellFormed::Yes && !document.document_element())
        return WebIDL::InvalidStateError::create(document.realm(), "Document has no document element"_string);

    // Otherwise, run the following steps:
    // 1. Let serialized document be an empty string.
    StringBuilder serialized_document;

    // 2. For each child child of node, in tree order, run the XML serialization algorithm on the child passing along the provided arguments, and append the result to serialized document.
    for (auto const* child = document.first_child(); child; child = child->next_sibling())
        serialized_document.append(TRY(serialize_node_to_xml_string_impl(*child, namespace_, namespace_prefix_map, prefix_index, require_well_formed)));

    // 3. Return the value of serialized document.
    return MUST(serialized_document.to_string());
}

// https://w3c.github.io/DOM-Parsing/#xml-serializing-a-comment-node
static WebIDL::ExceptionOr<String> serialize_comment(DOM::Comment const& comment, RequireWellFormed require_well_formed)
{
    // If the require well-formed flag is set (its value is true), and node's data contains characters that are not matched by the XML Char production
    // or contains "--" (two adjacent U+002D HYPHEN-MINUS characters) or that ends with a "-" (U+002D HYPHEN-MINUS) character, then throw an exception;
    // the serialization of this node's data would not be well-formed.
    if (require_well_formed == RequireWellFormed::Yes) {
        // FIXME: Check comment's data against the XML Char production.

        if (comment.data().contains("--"sv))
            return WebIDL::InvalidStateError::create(comment.realm(), "Comment data contains two adjacent hyphens"_string);

        if (comment.data().ends_with('-'))
            return WebIDL::InvalidStateError::create(comment.realm(), "Comment data ends with a hyphen"_string);
    }

    // Otherwise, return the concatenation of "<!--", node's data, and "-->".
    return MUST(String::formatted("<!--{}-->", comment.data()));
}

// https://w3c.github.io/DOM-Parsing/#xml-serializing-a-text-node
static WebIDL::ExceptionOr<String> serialize_text(DOM::Text const& text, [[maybe_unused]] RequireWellFormed require_well_formed)
{
    // FIXME: 1. If the require well-formed flag is set (its value is true), and node's data contains characters that are not matched by the XML Char production,
    //           then throw an exception; the serialization of this node's data would not be well-formed.

    // 2. Let markup be the value of node's data.
    auto markup = text.data();

    // 3. Replace any occurrences of "&" in markup by "&amp;".
    markup = MUST(markup.replace("&"sv, "&amp;"sv, ReplaceMode::All));

    // 4. Replace any occurrences of "<" in markup by "&lt;".
    markup = MUST(markup.replace("<"sv, "&lt;"sv, ReplaceMode::All));

    // 5. Replace any occurrences of ">" in markup by "&gt;".
    markup = MUST(markup.replace(">"sv, "&gt;"sv, ReplaceMode::All));

    // 6. Return the value of markup.
    return markup;
}

// https://w3c.github.io/DOM-Parsing/#xml-serializing-a-documentfragment-node
static WebIDL::ExceptionOr<String> serialize_document_fragment(DOM::DocumentFragment const& document_fragment, Optional<FlyString>& namespace_, HashMap<FlyString, Vector<Optional<FlyString>>>& namespace_prefix_map, u64& prefix_index, RequireWellFormed require_well_formed)
{
    // 1. Let markup the empty string.
    StringBuilder markup;

    // 2. For each child child of node, in tree order, run the XML serialization algorithm on the child given namespace, prefix map, a reference to prefix index,
    //    and flag require well-formed. Concatenate the result to markup.
    for (auto const* child = document_fragment.first_child(); child; child = child->next_sibling())
        markup.append(TRY(serialize_node_to_xml_string_impl(*child, namespace_, namespace_prefix_map, prefix_index, require_well_formed)));

    // 3. Return the value of markup.
    return MUST(markup.to_string());
}

// https://w3c.github.io/DOM-Parsing/#xml-serializing-a-documenttype-node
static WebIDL::ExceptionOr<String> serialize_document_type(DOM::DocumentType const& document_type, RequireWellFormed require_well_formed)
{
    if (require_well_formed == RequireWellFormed::Yes) {
        // FIXME: 1. If the require well-formed flag is true and the node's publicId attribute contains characters that are not matched by the XML PubidChar production,
        //           then throw an exception; the serialization of this node would not be a well-formed document type declaration.

        // 2. If the require well-formed flag is true and the node's systemId attribute contains characters that are not matched by the XML Char production or that contains
        //    both a """ (U+0022 QUOTATION MARK) and a "'" (U+0027 APOSTROPHE), then throw an exception; the serialization of this node would not be a well-formed document type declaration.
        // FIXME: Check systemId against the XML Char production.
        if (document_type.system_id().contains('"') && document_type.system_id().contains('\''))
            return WebIDL::InvalidStateError::create(document_type.realm(), "Document type system ID contains both a quotation mark and an apostrophe"_string);
    }

    // 3. Let markup be an empty string.
    StringBuilder markup;

    // 4. Append the string "<!DOCTYPE" to markup.
    // 5. Append " " (U+0020 SPACE) to markup.
    markup.append("<!DOCTYPE "sv);

    // 6. Append the value of the node's name attribute to markup. For a node belonging to an HTML document, the value will be all lowercase.
    markup.append(document_type.name());

    // 7. If the node's publicId is not the empty string then append the following, in the order listed, to markup:
    if (!document_type.public_id().is_empty()) {
        // 1. " " (U+0020 SPACE);
        // 2. The string "PUBLIC";
        // 3. " " (U+0020 SPACE);
        // 4. """ (U+0022 QUOTATION MARK);
        markup.append(" PUBLIC \""sv);

        // 5. The value of the node's publicId attribute;
        markup.append(document_type.public_id());

        // 6. """ (U+0022 QUOTATION MARK).
        markup.append('"');
    }

    // 8. If the node's systemId is not the empty string and the node's publicId is set to the empty string, then append the following, in the order listed, to markup:
    if (!document_type.system_id().is_empty() && document_type.public_id().is_empty()) {
        // 1. " " (U+0020 SPACE);
        // 2. The string "SYSTEM".
        markup.append(" SYSTEM"sv);
    }

    // 9. If the node's systemId is not the empty string then append the following, in the order listed, to markup:
    if (!document_type.system_id().is_empty()) {
        // 1. " " (U+0020 SPACE);
        // 2. """ (U+0022 QUOTATION MARK);
        markup.append(" \""sv);

        // 3. The value of the node's systemId attribute;
        markup.append(document_type.system_id());

        // 4. """ (U+0022 QUOTATION MARK).
        markup.append('"');
    }

    // 10. Append ">" (U+003E GREATER-THAN SIGN) to markup.
    markup.append('>');

    // 11. Return the value of markup.
    return MUST(markup.to_string());
}

// https://w3c.github.io/DOM-Parsing/#dfn-xml-serializing-a-processinginstruction-node
static WebIDL::ExceptionOr<String> serialize_processing_instruction(DOM::ProcessingInstruction const& processing_instruction, RequireWellFormed require_well_formed)
{
    if (require_well_formed == RequireWellFormed::Yes) {
        // 1. If the require well-formed flag is set (its value is true), and node's target contains a ":" (U+003A COLON) character
        //    or is an ASCII case-insensitive match for the string "xml", then throw an exception; the serialization of this node's target would not be well-formed.
        if (processing_instruction.target().contains(':'))
            return WebIDL::InvalidStateError::create(processing_instruction.realm(), "Processing instruction target contains a colon"_string);

        if (Infra::is_ascii_case_insensitive_match(processing_instruction.target(), "xml"sv))
            return WebIDL::InvalidStateError::create(processing_instruction.realm(), "Processing instruction target is equal to 'xml'"_string);

        // 2. If the require well-formed flag is set (its value is true), and node's data contains characters that are not matched by the XML Char production or contains
        //    the string "?>" (U+003F QUESTION MARK, U+003E GREATER-THAN SIGN), then throw an exception; the serialization of this node's data would not be well-formed.
        // FIXME: Check data against the XML Char production.
        if (processing_instruction.data().contains("?>"sv))
            return WebIDL::InvalidStateError::create(processing_instruction.realm(), "Processing instruction data contains a terminator"_string);
    }

    // 3. Let markup be the concatenation of the following, in the order listed:
    StringBuilder markup;

    // 1. "<?" (U+003C LESS-THAN SIGN, U+003F QUESTION MARK);
    markup.append("<?"sv);

    // 2. The value of node's target;
    markup.append(processing_instruction.target());

    // 3. " " (U+0020 SPACE);
    markup.append(' ');

    // 4. The value of node's data;
    markup.append(processing_instruction.data());

    // 5. "?>" (U+003F QUESTION MARK, U+003E GREATER-THAN SIGN).
    markup.append("?>"sv);

    // 4. Return the value of markup.
    return MUST(markup.to_string());
}

// FIXME: This is ad-hoc
static WebIDL::ExceptionOr<String> serialize_cdata_section(DOM::CDATASection const& cdata_section, RequireWellFormed require_well_formed)
{
    if (require_well_formed == RequireWellFormed::Yes && cdata_section.data().contains("]]>"sv))
        return WebIDL::InvalidStateError::create(cdata_section.realm(), "CDATA section data contains a CDATA section end delimiter"_string);

    StringBuilder markup;
    markup.append("<![CDATA["sv);
    markup.append(cdata_section.data());
    markup.append("]]>"sv);

    return MUST(markup.to_string());
}

}
