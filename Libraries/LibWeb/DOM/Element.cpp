/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022-2023, San Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/Debug.h>
#include <AK/StringBuilder.h>
#include <LibUnicode/CharacterTypes.h>
#include <LibUnicode/Locale.h>
#include <LibWeb/Animations/Animation.h>
#include <LibWeb/Bindings/ElementPrototype.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/SelectorEngine.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleValues/CSSKeywordValue.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/DOM/Attr.h>
#include <LibWeb/DOM/DOMTokenList.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/HTMLCollection.h>
#include <LibWeb/DOM/NamedNodeMap.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Geometry/DOMRect.h>
#include <LibWeb/Geometry/DOMRectList.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/CustomElements/CustomElementDefinition.h>
#include <LibWeb/HTML/CustomElements/CustomElementName.h>
#include <LibWeb/HTML/CustomElements/CustomElementReactionNames.h>
#include <LibWeb/HTML/CustomElements/CustomElementRegistry.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/HTMLAnchorElement.h>
#include <LibWeb/HTML/HTMLAreaElement.h>
#include <LibWeb/HTML/HTMLBodyElement.h>
#include <LibWeb/HTML/HTMLButtonElement.h>
#include <LibWeb/HTML/HTMLFieldSetElement.h>
#include <LibWeb/HTML/HTMLFrameSetElement.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLLIElement.h>
#include <LibWeb/HTML/HTMLMenuElement.h>
#include <LibWeb/HTML/HTMLOListElement.h>
#include <LibWeb/HTML/HTMLOptGroupElement.h>
#include <LibWeb/HTML/HTMLOptionElement.h>
#include <LibWeb/HTML/HTMLSelectElement.h>
#include <LibWeb/HTML/HTMLSlotElement.h>
#include <LibWeb/HTML/HTMLStyleElement.h>
#include <LibWeb/HTML/HTMLTableElement.h>
#include <LibWeb/HTML/HTMLTemplateElement.h>
#include <LibWeb/HTML/HTMLTextAreaElement.h>
#include <LibWeb/HTML/HTMLUListElement.h>
#include <LibWeb/HTML/Numbers.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/Scripting/Agent.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Infra/CharacterTypes.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/InlineNode.h>
#include <LibWeb/Layout/ListItemBox.h>
#include <LibWeb/Layout/TreeBuilder.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/ViewportPaintable.h>
#include <LibWeb/SVG/SVGAElement.h>
#include <LibWeb/Selection/Selection.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::DOM {

Element::Element(Document& document, DOM::QualifiedName qualified_name)
    : ParentNode(document, NodeType::ELEMENT_NODE)
    , m_qualified_name(move(qualified_name))
{
    make_html_uppercased_qualified_name();
}

Element::~Element() = default;

void Element::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Element);

    m_attributes = NamedNodeMap::create(*this);
}

void Element::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SlottableMixin::visit_edges(visitor);
    Animatable::visit_edges(visitor);

    visitor.visit(m_aria_active_descendant_element);
    visitor.visit(m_attributes);
    visitor.visit(m_inline_style);
    visitor.visit(m_class_list);
    visitor.visit(m_shadow_root);
    visitor.visit(m_custom_element_definition);
    visitor.visit(m_cascaded_properties);
    visitor.visit(m_computed_properties);
    if (m_pseudo_element_data) {
        for (auto& pseudo_element : *m_pseudo_element_data) {
            visitor.visit(pseudo_element.cascaded_properties);
            visitor.visit(pseudo_element.computed_properties);
            visitor.visit(pseudo_element.layout_node);
        }
    }
    if (m_registered_intersection_observers) {
        for (auto& registered_intersection_observers : *m_registered_intersection_observers)
            visitor.visit(registered_intersection_observers.observer);
    }
}

// https://dom.spec.whatwg.org/#dom-element-getattribute
Optional<String> Element::get_attribute(FlyString const& name) const
{
    // 1. Let attr be the result of getting an attribute given qualifiedName and this.
    auto const* attribute = m_attributes->get_attribute(name);

    // 2. If attr is null, return null.
    if (!attribute)
        return {};

    // 3. Return attr’s value.
    return attribute->value();
}

// https://dom.spec.whatwg.org/#dom-element-getattributens
Optional<String> Element::get_attribute_ns(Optional<FlyString> const& namespace_, FlyString const& name) const
{
    // 1. Let attr be the result of getting an attribute given namespace, localName, and this.
    auto const* attribute = m_attributes->get_attribute_ns(namespace_, name);

    // 2. If attr is null, return null.
    if (!attribute)
        return {};

    // 3. Return attr’s value.
    return attribute->value();
}

// https://dom.spec.whatwg.org/#concept-element-attributes-get-value
String Element::get_attribute_value(FlyString const& local_name, Optional<FlyString> const& namespace_) const
{
    // 1. Let attr be the result of getting an attribute given namespace, localName, and element.
    auto const* attribute = m_attributes->get_attribute_ns(namespace_, local_name);

    // 2. If attr is null, then return the empty string.
    if (!attribute)
        return String {};

    // 3. Return attr’s value.
    return attribute->value();
}

// https://dom.spec.whatwg.org/#dom-element-getattributenode
GC::Ptr<Attr> Element::get_attribute_node(FlyString const& name) const
{
    // The getAttributeNode(qualifiedName) method steps are to return the result of getting an attribute given qualifiedName and this.
    return m_attributes->get_attribute(name);
}

// https://dom.spec.whatwg.org/#dom-element-getattributenodens
GC::Ptr<Attr> Element::get_attribute_node_ns(Optional<FlyString> const& namespace_, FlyString const& name) const
{
    // The getAttributeNodeNS(namespace, localName) method steps are to return the result of getting an attribute given namespace, localName, and this.
    return m_attributes->get_attribute_ns(namespace_, name);
}

// https://dom.spec.whatwg.org/#dom-element-setattribute
WebIDL::ExceptionOr<void> Element::set_attribute(FlyString const& name, String const& value)
{
    // 1. If qualifiedName does not match the Name production in XML, then throw an "InvalidCharacterError" DOMException.
    if (!Document::is_valid_name(name.to_string()))
        return WebIDL::InvalidCharacterError::create(realm(), "Attribute name must not be empty or contain invalid characters"_string);

    // 2. If this is in the HTML namespace and its node document is an HTML document, then set qualifiedName to qualifiedName in ASCII lowercase.
    bool insert_as_lowercase = namespace_uri() == Namespace::HTML && document().document_type() == Document::Type::HTML;

    // 3. Let attribute be the first attribute in this’s attribute list whose qualified name is qualifiedName, and null otherwise.
    auto* attribute = m_attributes->get_attribute(name);

    // 4. If attribute is null, create an attribute whose local name is qualifiedName, value is value, and node document
    //    is this’s node document, then append this attribute to this, and then return.
    if (!attribute) {
        auto new_attribute = Attr::create(document(), insert_as_lowercase ? name.to_ascii_lowercase() : name, value);
        m_attributes->append_attribute(new_attribute);

        return {};
    }

    // 5. Change attribute to value.
    attribute->change_attribute(value);

    return {};
}

// https://dom.spec.whatwg.org/#validate-and-extract
WebIDL::ExceptionOr<QualifiedName> validate_and_extract(JS::Realm& realm, Optional<FlyString> namespace_, FlyString const& qualified_name)
{
    // To validate and extract a namespace and qualifiedName, run these steps:

    // 1. If namespace is the empty string, then set it to null.
    if (namespace_.has_value() && namespace_.value().is_empty())
        namespace_ = {};

    // 2. Validate qualifiedName.
    TRY(Document::validate_qualified_name(realm, qualified_name));

    // 3. Let prefix be null.
    Optional<FlyString> prefix = {};

    // 4. Let localName be qualifiedName.
    auto local_name = qualified_name;

    // 5. If qualifiedName contains a U+003A (:):
    if (qualified_name.bytes_as_string_view().contains(':')) {
        // 1. Let splitResult be the result of running strictly split given qualifiedName and U+003A (:).
        // FIXME: Use the "strictly split" algorithm
        auto split_result = qualified_name.bytes_as_string_view().split_view(':');

        // 2. Set prefix to splitResult[0].
        prefix = MUST(FlyString::from_utf8(split_result[0]));

        // 3. Set localName to splitResult[1].
        local_name = MUST(FlyString::from_utf8(split_result[1]));
    }

    // 6. If prefix is non-null and namespace is null, then throw a "NamespaceError" DOMException.
    if (prefix.has_value() && !namespace_.has_value())
        return WebIDL::NamespaceError::create(realm, "Prefix is non-null and namespace is null."_string);

    // 7. If prefix is "xml" and namespace is not the XML namespace, then throw a "NamespaceError" DOMException.
    if (prefix == "xml"sv && namespace_ != Namespace::XML)
        return WebIDL::NamespaceError::create(realm, "Prefix is 'xml' and namespace is not the XML namespace."_string);

    // 8. If either qualifiedName or prefix is "xmlns" and namespace is not the XMLNS namespace, then throw a "NamespaceError" DOMException.
    if ((qualified_name == "xmlns"sv || prefix == "xmlns"sv) && namespace_ != Namespace::XMLNS)
        return WebIDL::NamespaceError::create(realm, "Either qualifiedName or prefix is 'xmlns' and namespace is not the XMLNS namespace."_string);

    // 9. If namespace is the XMLNS namespace and neither qualifiedName nor prefix is "xmlns", then throw a "NamespaceError" DOMException.
    if (namespace_ == Namespace::XMLNS && !(qualified_name == "xmlns"sv || prefix == "xmlns"sv))
        return WebIDL::NamespaceError::create(realm, "Namespace is the XMLNS namespace and neither qualifiedName nor prefix is 'xmlns'."_string);

    // 10. Return namespace, prefix, and localName.
    return QualifiedName { local_name, prefix, namespace_ };
}

// https://dom.spec.whatwg.org/#dom-element-setattributens
WebIDL::ExceptionOr<void> Element::set_attribute_ns(Optional<FlyString> const& namespace_, FlyString const& qualified_name, String const& value)
{
    // 1. Let namespace, prefix, and localName be the result of passing namespace and qualifiedName to validate and extract.
    auto extracted_qualified_name = TRY(validate_and_extract(realm(), namespace_, qualified_name));

    // 2. Set an attribute value for this using localName, value, and also prefix and namespace.
    set_attribute_value(extracted_qualified_name.local_name(), value, extracted_qualified_name.prefix(), extracted_qualified_name.namespace_());

    return {};
}

// https://dom.spec.whatwg.org/#concept-element-attributes-append
void Element::append_attribute(FlyString const& name, String const& value)
{
    m_attributes->append_attribute(Attr::create(document(), name, value));
}

// https://dom.spec.whatwg.org/#concept-element-attributes-append
void Element::append_attribute(Attr& attribute)
{
    m_attributes->append_attribute(attribute);
}

// https://dom.spec.whatwg.org/#concept-element-attributes-set-value
void Element::set_attribute_value(FlyString const& local_name, String const& value, Optional<FlyString> const& prefix, Optional<FlyString> const& namespace_)
{
    // 1. Let attribute be the result of getting an attribute given namespace, localName, and element.
    auto* attribute = m_attributes->get_attribute_ns(namespace_, local_name);

    // 2. If attribute is null, create an attribute whose namespace is namespace, namespace prefix is prefix, local name
    //    is localName, value is value, and node document is element’s node document, then append this attribute to element,
    //    and then return.
    if (!attribute) {
        QualifiedName name { local_name, prefix, namespace_ };

        auto new_attribute = Attr::create(document(), move(name), value);
        m_attributes->append_attribute(new_attribute);

        return;
    }

    // 3. Change attribute to value.
    attribute->change_attribute(value);
}

// https://dom.spec.whatwg.org/#dom-element-setattributenode
WebIDL::ExceptionOr<GC::Ptr<Attr>> Element::set_attribute_node(Attr& attr)
{
    // The setAttributeNode(attr) and setAttributeNodeNS(attr) methods steps are to return the result of setting an attribute given attr and this.
    return m_attributes->set_attribute(attr);
}

// https://dom.spec.whatwg.org/#dom-element-setattributenodens
WebIDL::ExceptionOr<GC::Ptr<Attr>> Element::set_attribute_node_ns(Attr& attr)
{
    // The setAttributeNode(attr) and setAttributeNodeNS(attr) methods steps are to return the result of setting an attribute given attr and this.
    return m_attributes->set_attribute(attr);
}

// https://dom.spec.whatwg.org/#dom-element-removeattribute
void Element::remove_attribute(FlyString const& name)
{
    // The removeAttribute(qualifiedName) method steps are to remove an attribute given qualifiedName and this, and then return undefined.
    m_attributes->remove_attribute(name);
}

// https://dom.spec.whatwg.org/#dom-element-removeattributens
void Element::remove_attribute_ns(Optional<FlyString> const& namespace_, FlyString const& name)
{
    // The removeAttributeNS(namespace, localName) method steps are to remove an attribute given namespace, localName, and this, and then return undefined.
    m_attributes->remove_attribute_ns(namespace_, name);
}

// https://dom.spec.whatwg.org/#dom-element-removeattributenode
WebIDL::ExceptionOr<GC::Ref<Attr>> Element::remove_attribute_node(GC::Ref<Attr> attr)
{
    return m_attributes->remove_attribute_node(attr);
}

// https://dom.spec.whatwg.org/#dom-element-hasattribute
bool Element::has_attribute(FlyString const& name) const
{
    return m_attributes->get_attribute(name) != nullptr;
}

// https://dom.spec.whatwg.org/#dom-element-hasattributens
bool Element::has_attribute_ns(Optional<FlyString> const& namespace_, FlyString const& name) const
{
    // 1. If namespace is the empty string, then set it to null.
    // 2. Return true if this has an attribute whose namespace is namespace and local name is localName; otherwise false.
    if (namespace_ == FlyString {})
        return m_attributes->get_attribute_ns(OptionalNone {}, name) != nullptr;

    return m_attributes->get_attribute_ns(namespace_, name) != nullptr;
}

// https://dom.spec.whatwg.org/#dom-element-toggleattribute
WebIDL::ExceptionOr<bool> Element::toggle_attribute(FlyString const& name, Optional<bool> force)
{
    // 1. If qualifiedName does not match the Name production in XML, then throw an "InvalidCharacterError" DOMException.
    if (!Document::is_valid_name(name.to_string()))
        return WebIDL::InvalidCharacterError::create(realm(), "Attribute name must not be empty or contain invalid characters"_string);

    // 2. If this is in the HTML namespace and its node document is an HTML document, then set qualifiedName to qualifiedName in ASCII lowercase.
    bool insert_as_lowercase = namespace_uri() == Namespace::HTML && document().document_type() == Document::Type::HTML;

    // 3. Let attribute be the first attribute in this’s attribute list whose qualified name is qualifiedName, and null otherwise.
    auto* attribute = m_attributes->get_attribute(name);

    // 4. If attribute is null, then:
    if (!attribute) {
        // 1. If force is not given or is true, create an attribute whose local name is qualifiedName, value is the empty
        //    string, and node document is this’s node document, then append this attribute to this, and then return true.
        if (!force.has_value() || force.value()) {
            auto new_attribute = Attr::create(document(), insert_as_lowercase ? name.to_ascii_lowercase() : name.to_string(), String {});
            m_attributes->append_attribute(new_attribute);

            return true;
        }

        // 2. Return false.
        return false;
    }

    // 5. Otherwise, if force is not given or is false, remove an attribute given qualifiedName and this, and then return false.
    if (!force.has_value() || !force.value()) {
        m_attributes->remove_attribute(name);
        return false;
    }

    // 6. Return true.
    return true;
}

// https://dom.spec.whatwg.org/#dom-element-getattributenames
Vector<String> Element::get_attribute_names() const
{
    // The getAttributeNames() method steps are to return the qualified names of the attributes in this’s attribute list, in order; otherwise a new list.
    Vector<String> names;
    for (size_t i = 0; i < m_attributes->length(); ++i) {
        auto const* attribute = m_attributes->item(i);
        names.append(attribute->name().to_string());
    }
    return names;
}

GC::Ptr<Layout::Node> Element::create_layout_node(GC::Ref<CSS::ComputedProperties> style)
{
    if (local_name() == "noscript" && document().is_scripting_enabled())
        return nullptr;

    auto display = style->display();
    return create_layout_node_for_display_type(document(), display, style, this);
}

GC::Ptr<Layout::NodeWithStyle> Element::create_layout_node_for_display_type(DOM::Document& document, CSS::Display const& display, GC::Ref<CSS::ComputedProperties> style, Element* element)
{
    if (display.is_table_inside() || display.is_table_row_group() || display.is_table_header_group() || display.is_table_footer_group() || display.is_table_row())
        return document.heap().allocate<Layout::Box>(document, element, move(style));

    if (display.is_list_item())
        return document.heap().allocate<Layout::ListItemBox>(document, element, move(style));

    if (display.is_table_cell())
        return document.heap().allocate<Layout::BlockContainer>(document, element, move(style));

    if (display.is_table_column() || display.is_table_column_group() || display.is_table_caption()) {
        // FIXME: This is just an incorrect placeholder until we improve table layout support.
        return document.heap().allocate<Layout::BlockContainer>(document, element, move(style));
    }

    if (display.is_math_inside()) {
        // https://w3c.github.io/mathml-core/#new-display-math-value
        // MathML elements with a computed display value equal to block math or inline math control box generation
        // and layout according to their tag name, as described in the relevant sections.
        // FIXME: Figure out what kind of node we should make for them. For now, we'll stick with a generic Box.
        return document.heap().allocate<Layout::BlockContainer>(document, element, move(style));
    }

    if (display.is_inline_outside()) {
        if (display.is_flow_root_inside())
            return document.heap().allocate<Layout::BlockContainer>(document, element, move(style));
        if (display.is_flow_inside())
            return document.heap().allocate<Layout::InlineNode>(document, element, move(style));
        if (display.is_flex_inside())
            return document.heap().allocate<Layout::Box>(document, element, move(style));
        if (display.is_grid_inside())
            return document.heap().allocate<Layout::Box>(document, element, move(style));
        dbgln_if(LIBWEB_CSS_DEBUG, "FIXME: Support display: {}", display.to_string());
        return document.heap().allocate<Layout::InlineNode>(document, element, move(style));
    }

    if (display.is_flex_inside() || display.is_grid_inside())
        return document.heap().allocate<Layout::Box>(document, element, move(style));

    if (display.is_flow_inside() || display.is_flow_root_inside() || display.is_contents())
        return document.heap().allocate<Layout::BlockContainer>(document, element, move(style));

    dbgln("FIXME: CSS display '{}' not implemented yet.", display.to_string());

    // FIXME: We don't actually support `display: block ruby`, this is just a hack to prevent a crash
    if (display.is_ruby_inside())
        return document.heap().allocate<Layout::BlockContainer>(document, element, move(style));

    return document.heap().allocate<Layout::InlineNode>(document, element, move(style));
}

void Element::run_attribute_change_steps(FlyString const& local_name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    attribute_changed(local_name, old_value, value, namespace_);

    if (old_value != value) {
        invalidate_style_after_attribute_change(local_name, old_value, value);
        document().bump_dom_tree_version();
    }
}

static CSS::RequiredInvalidationAfterStyleChange compute_required_invalidation(CSS::ComputedProperties const& old_style, CSS::ComputedProperties const& new_style)
{
    CSS::RequiredInvalidationAfterStyleChange invalidation;

    if (!old_style.computed_font_list().equals(new_style.computed_font_list()))
        invalidation.relayout = true;

    for (auto i = to_underlying(CSS::first_property_id); i <= to_underlying(CSS::last_property_id); ++i) {
        auto property_id = static_cast<CSS::PropertyID>(i);
        auto old_value = old_style.maybe_null_property(property_id);
        auto new_value = new_style.maybe_null_property(property_id);
        if (!old_value && !new_value)
            continue;

        invalidation |= CSS::compute_property_invalidation(property_id, old_value, new_value);
    }
    return invalidation;
}

CSS::RequiredInvalidationAfterStyleChange Element::recompute_style()
{
    VERIFY(parent());

    m_affected_by_has_pseudo_class_in_subject_position = false;
    m_affected_by_has_pseudo_class_in_non_subject_position = false;
    m_affected_by_has_pseudo_class_with_relative_selector_that_has_sibling_combinator = false;
    m_affected_by_direct_sibling_combinator = false;
    m_affected_by_indirect_sibling_combinator = false;
    m_affected_by_first_or_last_child_pseudo_class = false;
    m_affected_by_nth_child_pseudo_class = false;
    m_sibling_invalidation_distance = 0;

    auto& style_computer = document().style_computer();
    auto new_computed_properties = style_computer.compute_style(*this);

    // Tables must not inherit -libweb-* values for text-align.
    // FIXME: Find the spec for this.
    if (is<HTML::HTMLTableElement>(*this)) {
        auto text_align = new_computed_properties->text_align();
        if (text_align == CSS::TextAlign::LibwebLeft || text_align == CSS::TextAlign::LibwebCenter || text_align == CSS::TextAlign::LibwebRight)
            new_computed_properties->set_property(CSS::PropertyID::TextAlign, CSS::CSSKeywordValue::create(CSS::Keyword::Start));
    }

    bool had_list_marker = false;

    CSS::RequiredInvalidationAfterStyleChange invalidation;
    if (m_computed_properties) {
        invalidation = compute_required_invalidation(*m_computed_properties, new_computed_properties);
        had_list_marker = m_computed_properties->display().is_list_item();
    } else {
        invalidation = CSS::RequiredInvalidationAfterStyleChange::full();
    }

    auto old_display_is_none = m_computed_properties ? m_computed_properties->display().is_none() : true;
    auto new_display_is_none = new_computed_properties->display().is_none();

    if (!invalidation.is_none())
        set_computed_properties(move(new_computed_properties));

    if (old_display_is_none != new_display_is_none) {
        play_or_cancel_animations_after_display_property_change();
    }

    // Any document change that can cause this element's style to change, could also affect its pseudo-elements.
    auto recompute_pseudo_element_style = [&](CSS::PseudoElement pseudo_element) {
        style_computer.push_ancestor(*this);

        auto pseudo_element_style = pseudo_element_computed_properties(pseudo_element);
        auto new_pseudo_element_style = style_computer.compute_pseudo_element_style_if_needed(*this, pseudo_element);

        // TODO: Can we be smarter about invalidation?
        if (pseudo_element_style && new_pseudo_element_style) {
            invalidation |= compute_required_invalidation(*pseudo_element_style, *new_pseudo_element_style);
        } else if (pseudo_element_style || new_pseudo_element_style) {
            invalidation = CSS::RequiredInvalidationAfterStyleChange::full();
        }

        set_pseudo_element_computed_properties(pseudo_element, move(new_pseudo_element_style));
        style_computer.pop_ancestor(*this);
    };

    recompute_pseudo_element_style(CSS::PseudoElement::Before);
    recompute_pseudo_element_style(CSS::PseudoElement::After);
    if (had_list_marker || m_computed_properties->display().is_list_item())
        recompute_pseudo_element_style(CSS::PseudoElement::Marker);

    if (invalidation.is_none())
        return invalidation;

    if (invalidation.repaint)
        document().set_needs_to_resolve_paint_only_properties();

    if (!invalidation.rebuild_layout_tree && layout_node()) {
        // If we're keeping the layout tree, we can just apply the new style to the existing layout tree.
        layout_node()->apply_style(*m_computed_properties);
        if (invalidation.repaint && paintable())
            paintable()->set_needs_display();

        // Do the same for pseudo-elements.
        for (auto i = 0; i < to_underlying(CSS::PseudoElement::KnownPseudoElementCount); i++) {
            auto pseudo_element_type = static_cast<CSS::PseudoElement>(i);
            auto pseudo_element = get_pseudo_element(pseudo_element_type);
            if (!pseudo_element.has_value() || !pseudo_element->layout_node)
                continue;

            auto pseudo_element_style = pseudo_element_computed_properties(pseudo_element_type);
            if (!pseudo_element_style)
                continue;

            if (auto* node_with_style = dynamic_cast<Layout::NodeWithStyle*>(pseudo_element->layout_node.ptr())) {
                node_with_style->apply_style(*pseudo_element_style);
                if (invalidation.repaint && node_with_style->first_paintable())
                    node_with_style->first_paintable()->set_needs_display();
            }
        }
    }

    return invalidation;
}

CSS::RequiredInvalidationAfterStyleChange Element::recompute_inherited_style()
{
    auto computed_properties = this->computed_properties();
    if (!m_cascaded_properties || !computed_properties || !layout_node())
        return {};

    CSS::RequiredInvalidationAfterStyleChange invalidation;

    HashMap<size_t, RefPtr<CSS::CSSStyleValue const>> old_values_with_relative_units;
    for (auto i = to_underlying(CSS::first_property_id); i <= to_underlying(CSS::last_property_id); ++i) {
        auto property_id = static_cast<CSS::PropertyID>(i);
        auto const& preabsolutized_value = m_cascaded_properties->property(property_id);
        RefPtr old_value = computed_properties->maybe_null_property(property_id);
        // Update property if it uses relative units as it might have been affected by a change in ancestor element style.
        if (preabsolutized_value && preabsolutized_value->is_length() && preabsolutized_value->as_length().length().is_font_relative()) {
            auto is_inherited = computed_properties->is_property_inherited(property_id);
            computed_properties->set_property(property_id, *preabsolutized_value, is_inherited ? CSS::ComputedProperties::Inherited::Yes : CSS::ComputedProperties::Inherited::No);
            old_values_with_relative_units.set(i, old_value);
        }
        if (!computed_properties->is_property_inherited(property_id))
            continue;
        RefPtr new_value = CSS::StyleComputer::get_inherit_value(property_id, this);
        computed_properties->set_property(property_id, *new_value, CSS::ComputedProperties::Inherited::Yes);
        invalidation |= CSS::compute_property_invalidation(property_id, old_value, new_value);
    }

    if (invalidation.is_none() && old_values_with_relative_units.is_empty())
        return invalidation;

    document().style_computer().compute_font(*computed_properties, this, {});
    document().style_computer().absolutize_values(*computed_properties, this);

    for (auto [property_id, old_value] : old_values_with_relative_units) {
        auto new_value = computed_properties->maybe_null_property(static_cast<CSS::PropertyID>(property_id));
        invalidation |= CSS::compute_property_invalidation(static_cast<CSS::PropertyID>(property_id), old_value, new_value);
    }

    if (invalidation.is_none())
        return invalidation;

    layout_node()->apply_style(*computed_properties);
    return invalidation;
}

GC::Ref<CSS::ComputedProperties> Element::resolved_css_values(Optional<CSS::PseudoElement> type)
{
    auto element_computed_style = CSS::CSSStyleProperties::create_resolved_style({ *this, type });
    auto properties = heap().allocate<CSS::ComputedProperties>();

    for (auto i = to_underlying(CSS::first_property_id); i <= to_underlying(CSS::last_property_id); ++i) {
        auto property_id = (CSS::PropertyID)i;
        auto maybe_value = element_computed_style->property(property_id);
        if (!maybe_value.has_value())
            continue;
        properties->set_property(property_id, maybe_value.release_value().value);
    }

    return properties;
}

void Element::reset_animated_css_properties()
{
    if (!m_computed_properties)
        return;
    m_computed_properties->reset_animated_properties();
}

DOMTokenList* Element::class_list()
{
    if (!m_class_list)
        m_class_list = DOMTokenList::create(*this, HTML::AttributeNames::class_);
    return m_class_list;
}

// https://dom.spec.whatwg.org/#valid-shadow-host-name
static bool is_valid_shadow_host_name(FlyString const& name)
{
    // A valid shadow host name is:
    // - a valid custom element name
    // - "article", "aside", "blockquote", "body", "div", "footer", "h1", "h2", "h3", "h4", "h5", "h6", "header", "main", "nav", "p", "section", or "span"
    if (!HTML::is_valid_custom_element_name(name)
        && !name.is_one_of("article", "aside", "blockquote", "body", "div", "footer", "h1", "h2", "h3", "h4", "h5", "h6", "header", "main", "nav", "p", "section", "span")) {
        return false;
    }
    return true;
}

// https://dom.spec.whatwg.org/#concept-attach-a-shadow-root
WebIDL::ExceptionOr<void> Element::attach_a_shadow_root(Bindings::ShadowRootMode mode, bool clonable, bool serializable, bool delegates_focus, Bindings::SlotAssignmentMode slot_assignment)
{
    // 1. If element’s namespace is not the HTML namespace, then throw a "NotSupportedError" DOMException.
    if (namespace_uri() != Namespace::HTML)
        return WebIDL::NotSupportedError::create(realm(), "Element's namespace is not the HTML namespace"_string);

    // 2. If element’s local name is not a valid shadow host name, then throw a "NotSupportedError" DOMException.
    if (!is_valid_shadow_host_name(local_name()))
        return WebIDL::NotSupportedError::create(realm(), "Element's local name is not a valid shadow host name"_string);

    // 3. If element’s local name is a valid custom element name, or element’s is value is not null, then:
    if (HTML::is_valid_custom_element_name(local_name()) || m_is_value.has_value()) {
        // 1. Let definition be the result of looking up a custom element definition given element’s node document, its namespace, its local name, and its is value.
        auto definition = document().lookup_custom_element_definition(namespace_uri(), local_name(), m_is_value);

        // 2. If definition is not null and definition’s disable shadow is true, then throw a "NotSupportedError" DOMException.
        if (definition && definition->disable_shadow())
            return WebIDL::NotSupportedError::create(realm(), "Cannot attach a shadow root to a custom element that has disabled shadow roots"_string);
    }

    // 4. If element is a shadow host, then:
    if (is_shadow_host()) {
        // 1. Let currentShadowRoot be element’s shadow root.
        auto current_shadow_root = shadow_root();

        // 2. If any of the following are true:
        // - currentShadowRoot’s declarative is false; or
        // - currentShadowRoot’s mode is not mode,
        // then throw a "NotSupportedError" DOMException.
        if (!current_shadow_root->declarative() || current_shadow_root->mode() != mode) {
            return WebIDL::NotSupportedError::create(realm(), "Element already is a shadow host"_string);
        }

        // 3. Otherwise:
        //    1. Remove all of currentShadowRoot’s children, in tree order.
        current_shadow_root->remove_all_children();

        //    2. Set currentShadowRoot’s declarative to false.
        current_shadow_root->set_declarative(false);

        //    3. Return.
        return {};
    }

    // 5. Let shadow be a new shadow root whose node document is element’s node document, host is this, and mode is mode.
    auto shadow = realm().create<ShadowRoot>(document(), *this, mode);

    // 6. Set shadow’s delegates focus to delegatesFocus".
    shadow->set_delegates_focus(delegates_focus);

    // 7. If element’s custom element state is "precustomized" or "custom", then set shadow’s available to element internals to true.
    if (m_custom_element_state == CustomElementState::Precustomized || m_custom_element_state == CustomElementState::Custom)
        shadow->set_available_to_element_internals(true);

    // 8. Set shadow’s slot assignment to slotAssignment.
    shadow->set_slot_assignment(slot_assignment);

    // 9. Set shadow’s declarative to false.
    shadow->set_declarative(false);

    // 10. Set shadow’s clonable to clonable.
    shadow->set_clonable(clonable);

    // 11. Set shadow’s serializable to serializable.
    shadow->set_serializable(serializable);

    // 12. Set element’s shadow root to shadow.
    set_shadow_root(shadow);
    return {};
}

// https://dom.spec.whatwg.org/#dom-element-attachshadow
WebIDL::ExceptionOr<GC::Ref<ShadowRoot>> Element::attach_shadow(ShadowRootInit init)
{
    // 1. Run attach a shadow root with this, init["mode"], init["clonable"], init["serializable"], init["delegatesFocus"], and init["slotAssignment"].
    TRY(attach_a_shadow_root(init.mode, init.clonable, init.serializable, init.delegates_focus, init.slot_assignment));

    // 2. Return this’s shadow root.
    return GC::Ref { *shadow_root() };
}

// https://dom.spec.whatwg.org/#dom-element-shadowroot
GC::Ptr<ShadowRoot> Element::shadow_root_for_bindings() const
{
    // 1. Let shadow be this’s shadow root.
    auto shadow = m_shadow_root;

    // 2. If shadow is null or its mode is "closed", then return null.
    if (shadow == nullptr || shadow->mode() == Bindings::ShadowRootMode::Closed)
        return nullptr;

    // 3. Return shadow.
    return shadow;
}

// https://dom.spec.whatwg.org/#dom-element-matches
WebIDL::ExceptionOr<bool> Element::matches(StringView selectors) const
{
    // 1. Let s be the result of parse a selector from selectors.
    auto maybe_selectors = parse_selector(CSS::Parser::ParsingParams(document()), selectors);

    // 2. If s is failure, then throw a "SyntaxError" DOMException.
    if (!maybe_selectors.has_value())
        return WebIDL::SyntaxError::create(realm(), "Failed to parse selector"_string);

    // 3. If the result of match a selector against an element, using s, this, and scoping root this, returns success, then return true; otherwise, return false.
    auto sel = maybe_selectors.value();
    for (auto& s : sel) {
        SelectorEngine::MatchContext context;
        if (SelectorEngine::matches(s, *this, nullptr, context, {}, static_cast<ParentNode const*>(this)))
            return true;
    }
    return false;
}

// https://dom.spec.whatwg.org/#dom-element-closest
WebIDL::ExceptionOr<DOM::Element const*> Element::closest(StringView selectors) const
{
    // 1. Let s be the result of parse a selector from selectors.
    auto maybe_selectors = parse_selector(CSS::Parser::ParsingParams(document()), selectors);

    // 2. If s is failure, then throw a "SyntaxError" DOMException.
    if (!maybe_selectors.has_value())
        return WebIDL::SyntaxError::create(realm(), "Failed to parse selector"_string);

    auto matches_selectors = [this](CSS::SelectorList const& selector_list, Element const* element) {
        // 4. For each element in elements, if match a selector against an element, using s, element, and scoping root this, returns success, return element.
        for (auto const& selector : selector_list) {
            SelectorEngine::MatchContext context;
            if (SelectorEngine::matches(selector, *element, nullptr, context, {}, this))
                return true;
        }
        return false;
    };

    auto const selector_list = maybe_selectors.release_value();

    // 3. Let elements be this’s inclusive ancestors that are elements, in reverse tree order.
    for (auto* element = this; element; element = element->parent_element()) {
        if (!matches_selectors(selector_list, element))
            continue;

        return element;
    }

    // 5. Return null.
    return nullptr;
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-element-innerhtml
WebIDL::ExceptionOr<void> Element::set_inner_html(StringView value)
{
    // FIXME: 1. Let compliantString be the result of invoking the Get Trusted Type compliant string algorithm with TrustedHTML, this's relevant global object, the given value, "Element innerHTML", and "script".

    // 2. Let context be this.
    DOM::Node* context = this;

    // 3. Let fragment be the result of invoking the fragment parsing algorithm steps with context and compliantString. FIXME: Use compliantString.
    auto fragment = TRY(as<Element>(*context).parse_fragment(value));

    // 4. If context is a template element, then set context to the template element's template contents (a DocumentFragment).
    if (is<HTML::HTMLTemplateElement>(*context))
        context = as<HTML::HTMLTemplateElement>(*context).content();

    // 5. Replace all with fragment within context.
    context->replace_all(fragment);

    // NOTE: We don't invalidate style & layout for <template> elements since they don't affect rendering.
    if (!is<HTML::HTMLTemplateElement>(*context)) {
        context->set_needs_style_update(true);

        if (context->is_connected()) {
            // NOTE: Since the DOM has changed, we have to rebuild the layout tree.
            context->set_needs_layout_tree_update(true);
        }
    }

    return {};
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-element-innerhtml
WebIDL::ExceptionOr<String> Element::inner_html() const
{
    return serialize_fragment(HTML::RequireWellFormed::Yes);
}

bool Element::is_focused() const
{
    return document().focused_element() == this;
}

bool Element::is_active() const
{
    return document().active_element() == this;
}

bool Element::is_target() const
{
    return document().target_element() == this;
}

// https://dom.spec.whatwg.org/#document-element
bool Element::is_document_element() const
{
    // The document element of a document is the element whose parent is that document, if it exists; otherwise null.
    return parent() == &document();
}

// https://dom.spec.whatwg.org/#element-shadow-host
bool Element::is_shadow_host() const
{
    // An element is a shadow host if its shadow root is non-null.
    return m_shadow_root != nullptr;
}

void Element::set_shadow_root(GC::Ptr<ShadowRoot> shadow_root)
{
    if (m_shadow_root == shadow_root)
        return;
    if (m_shadow_root)
        m_shadow_root->set_host(nullptr);
    m_shadow_root = move(shadow_root);
    if (m_shadow_root)
        m_shadow_root->set_host(this);
    invalidate_style(StyleInvalidationReason::ElementSetShadowRoot);
}

GC::Ref<CSS::CSSStyleProperties> Element::style_for_bindings()
{
    if (!m_inline_style)
        m_inline_style = CSS::CSSStyleProperties::create_element_inline_style({ *this }, {}, {});
    return *m_inline_style;
}

void Element::set_inline_style(GC::Ptr<CSS::CSSStyleProperties> style)
{
    m_inline_style = style;
    set_needs_style_update(true);
}

// https://dom.spec.whatwg.org/#element-html-uppercased-qualified-name
void Element::make_html_uppercased_qualified_name()
{
    // This is allowed by the spec: "User agents could optimize qualified name and HTML-uppercased qualified name by storing them in internal slots."
    if (namespace_uri() == Namespace::HTML && document().document_type() == Document::Type::HTML)
        m_html_uppercased_qualified_name = qualified_name().to_ascii_uppercase();
    else
        m_html_uppercased_qualified_name = qualified_name();
}

// https://html.spec.whatwg.org/multipage/webappapis.html#queue-an-element-task
HTML::TaskID Element::queue_an_element_task(HTML::Task::Source source, Function<void()> steps)
{
    return queue_a_task(source, HTML::main_thread_event_loop(), document(), GC::create_function(heap(), move(steps)));
}

// https://html.spec.whatwg.org/multipage/syntax.html#void-elements
bool Element::is_void_element() const
{
    return local_name().is_one_of(HTML::TagNames::area, HTML::TagNames::base, HTML::TagNames::br, HTML::TagNames::col, HTML::TagNames::embed, HTML::TagNames::hr, HTML::TagNames::img, HTML::TagNames::input, HTML::TagNames::link, HTML::TagNames::meta, HTML::TagNames::param, HTML::TagNames::source, HTML::TagNames::track, HTML::TagNames::wbr);
}

// https://html.spec.whatwg.org/multipage/parsing.html#serializes-as-void
bool Element::serializes_as_void() const
{
    return is_void_element() || local_name().is_one_of(HTML::TagNames::basefont, HTML::TagNames::bgsound, HTML::TagNames::frame, HTML::TagNames::keygen);
}

// https://drafts.csswg.org/cssom-view/#dom-element-getboundingclientrect
GC::Ref<Geometry::DOMRect> Element::get_bounding_client_rect_for_bindings() const
{
    auto rect = get_bounding_client_rect();
    return MUST(Geometry::DOMRect::construct_impl(realm(), static_cast<double>(rect.x()), static_cast<double>(rect.y()), static_cast<double>(rect.width()), static_cast<double>(rect.height())));
}

// https://drafts.csswg.org/cssom-view/#dom-element-getboundingclientrect
CSSPixelRect Element::get_bounding_client_rect() const
{
    // 1. Let list be the result of invoking getClientRects() on element.
    auto list = get_client_rects();

    // 2. If the list is empty return a DOMRect object whose x, y, width and height members are zero.
    if (list.size() == 0)
        return { 0, 0, 0, 0 };

    // 3. If all rectangles in list have zero width or height, return the first rectangle in list.
    auto all_rectangle_has_zero_width_or_height = true;
    for (auto i = 0u; i < list.size(); ++i) {
        auto const& rect = list.at(i);
        if (rect.width() != 0 && rect.height() != 0) {
            all_rectangle_has_zero_width_or_height = false;
            break;
        }
    }
    if (all_rectangle_has_zero_width_or_height)
        return list.at(0);

    // 4. Otherwise, return a DOMRect object describing the smallest rectangle that includes all of the rectangles in
    //    list of which the height or width is not zero.
    auto bounding_rect = list.at(0);
    for (auto i = 1u; i < list.size(); ++i) {
        auto const& rect = list.at(i);
        if (rect.width() == 0 || rect.height() == 0)
            continue;
        bounding_rect.unite(rect);
    }
    return bounding_rect;
}

// https://drafts.csswg.org/cssom-view/#dom-element-getclientrects
GC::Ref<Geometry::DOMRectList> Element::get_client_rects_for_bindings() const
{
    Vector<GC::Root<Geometry::DOMRect>> rects;
    for (auto const& rect : get_client_rects()) {
        rects.append(MUST(Geometry::DOMRect::construct_impl(realm(), static_cast<double>(rect.x()), static_cast<double>(rect.y()), static_cast<double>(rect.width()), static_cast<double>(rect.height()))));
    }
    return Geometry::DOMRectList::create(realm(), move(rects));
}

// https://drafts.csswg.org/cssom-view/#dom-element-getclientrects
Vector<CSSPixelRect> Element::get_client_rects() const
{
    auto navigable = document().navigable();
    if (!navigable)
        return {};

    // NOTE: Ensure that layout is up-to-date before looking at metrics.
    const_cast<Document&>(document()).update_layout(UpdateLayoutReason::ElementGetClientRects);

    // 1. If the element on which it was invoked does not have an associated layout box return an empty DOMRectList
    //    object and stop this algorithm.
    if (!layout_node())
        return {};

    // FIXME: 2. If the element has an associated SVG layout box return a DOMRectList object containing a single
    //          DOMRect object that describes the bounding box of the element as defined by the SVG specification,
    //          applying the transforms that apply to the element and its ancestors.

    // 3. Return a DOMRectList object containing DOMRect objects in content order, one for each box fragment,
    // describing its border area (including those with a height or width of zero) with the following constraints:
    // - Apply the transforms that apply to the element and its ancestors.
    // FIXME: - If the element on which the method was invoked has a computed value for the display property of table
    //          or inline-table include both the table box and the caption box, if any, but not the anonymous container box.
    // FIXME: - Replace each anonymous block box with its child box(es) and repeat this until no anonymous block boxes
    //          are left in the final list.

    // NOTE: Make sure CSS transforms are resolved before it is used to calculate the rect position.
    const_cast<Document&>(document()).update_paint_and_hit_testing_properties_if_needed();

    Gfx::AffineTransform transform;
    CSSPixelPoint scroll_offset;
    auto const* paintable = this->paintable();

    Vector<CSSPixelRect> rects;
    if (auto const* paintable_box = this->paintable_box()) {
        transform = Gfx::extract_2d_affine_transform(paintable_box->transform());
        for (auto const* containing_block = paintable->containing_block(); !containing_block->is_viewport(); containing_block = containing_block->containing_block()) {
            transform = Gfx::extract_2d_affine_transform(containing_block->transform()).multiply(transform);
        }

        if (auto enclosing_scroll_offset = paintable_box->enclosing_scroll_frame(); enclosing_scroll_offset) {
            scroll_offset.translate_by(-enclosing_scroll_offset->cumulative_offset());
        }

        auto absolute_rect = paintable_box->absolute_border_box_rect();
        auto transformed_rect = transform.map(absolute_rect.translated(-paintable_box->transform_origin()).to_type<float>())
                                    .to_type<CSSPixels>()
                                    .translated(paintable_box->transform_origin())
                                    .translated(-scroll_offset);
        rects.append(transformed_rect);
    } else if (paintable) {
        dbgln("FIXME: Failed to get client rects for element ({})", debug_description());
    }

    return rects;
}

int Element::client_top() const
{
    // NOTE: Ensure that layout is up-to-date before looking at metrics.
    const_cast<Document&>(document()).update_layout(UpdateLayoutReason::ElementClientTop);

    // 1. If the element has no associated CSS layout box or if the CSS layout box is inline, return zero.
    if (!paintable_box())
        return 0;

    // 2. Return the computed value of the border-top-width property
    //    plus the height of any scrollbar rendered between the top padding edge and the top border edge,
    //    ignoring any transforms that apply to the element and its ancestors.
    return paintable_box()->computed_values().border_top().width.to_int();
}

// https://drafts.csswg.org/cssom-view/#dom-element-clientleft
int Element::client_left() const
{
    // NOTE: Ensure that layout is up-to-date before looking at metrics.
    const_cast<Document&>(document()).update_layout(UpdateLayoutReason::ElementClientLeft);

    // 1. If the element has no associated CSS layout box or if the CSS layout box is inline, return zero.
    if (!paintable_box())
        return 0;

    // 2. Return the computed value of the border-left-width property
    //    plus the width of any scrollbar rendered between the left padding edge and the left border edge,
    //    ignoring any transforms that apply to the element and its ancestors.
    return paintable_box()->computed_values().border_left().width.to_int();
}

// https://drafts.csswg.org/cssom-view/#dom-element-clientwidth
int Element::client_width() const
{
    // NOTE: We do step 2 before step 1 here since step 2 can exit early without needing to perform layout.

    // 2. If the element is the root element and the element’s node document is not in quirks mode,
    //    or if the element is the HTML body element and the element’s node document is in quirks mode,
    //    return the viewport width excluding the size of a rendered scroll bar (if any).
    if ((is<HTML::HTMLHtmlElement>(*this) && !document().in_quirks_mode())
        || (is<HTML::HTMLBodyElement>(*this) && document().in_quirks_mode())) {
        return document().viewport_rect().width().to_int();
    }

    // NOTE: Ensure that layout is up-to-date before looking at metrics.
    const_cast<Document&>(document()).update_layout(UpdateLayoutReason::ElementClientWidth);

    // 1. If the element has no associated CSS layout box or if the CSS layout box is inline, return zero.
    if (!paintable_box())
        return 0;

    // 3. Return the width of the padding edge excluding the width of any rendered scrollbar between the padding edge and the border edge,
    // ignoring any transforms that apply to the element and its ancestors.
    return paintable_box()->absolute_padding_box_rect().width().to_int();
}

// https://drafts.csswg.org/cssom-view/#dom-element-clientheight
int Element::client_height() const
{
    // NOTE: We do step 2 before step 1 here since step 2 can exit early without needing to perform layout.

    // 2. If the element is the root element and the element’s node document is not in quirks mode,
    //    or if the element is the HTML body element and the element’s node document is in quirks mode,
    //    return the viewport height excluding the size of a rendered scroll bar (if any).
    if ((is<HTML::HTMLHtmlElement>(*this) && !document().in_quirks_mode())
        || (is<HTML::HTMLBodyElement>(*this) && document().in_quirks_mode())) {
        return document().viewport_rect().height().to_int();
    }

    // NOTE: Ensure that layout is up-to-date before looking at metrics.
    const_cast<Document&>(document()).update_layout(UpdateLayoutReason::ElementClientHeight);

    // 1. If the element has no associated CSS layout box or if the CSS layout box is inline, return zero.
    if (!paintable_box())
        return 0;

    // 3. Return the height of the padding edge excluding the height of any rendered scrollbar between the padding edge and the border edge,
    //    ignoring any transforms that apply to the element and its ancestors.
    return paintable_box()->absolute_padding_box_rect().height().to_int();
}

// https://drafts.csswg.org/cssom-view/#dom-element-currentcsszoom
double Element::current_css_zoom() const
{
    dbgln("FIXME: Implement Element::current_css_zoom()");
    return 1.0;
}

void Element::inserted()
{
    Base::inserted();

    if (is_connected()) {
        if (m_id.has_value())
            document().element_with_id_was_added({}, *this);
        if (m_name.has_value())
            document().element_with_name_was_added({}, *this);
    }
}

void Element::removed_from(Node* old_parent, Node& old_root)
{
    Base::removed_from(old_parent, old_root);

    if (old_root.is_connected()) {
        if (m_id.has_value())
            document().element_with_id_was_removed({}, *this);
        if (m_name.has_value())
            document().element_with_name_was_removed({}, *this);
    }
}

void Element::children_changed(ChildrenChangedMetadata const* metadata)
{
    Node::children_changed(metadata);
    set_needs_style_update(true);
}

void Element::set_pseudo_element_node(Badge<Layout::TreeBuilder>, CSS::PseudoElement pseudo_element, GC::Ptr<Layout::NodeWithStyle> pseudo_element_node)
{
    auto existing_pseudo_element = get_pseudo_element(pseudo_element);
    if (!existing_pseudo_element.has_value() && !pseudo_element_node)
        return;

    if (!CSS::Selector::PseudoElementSelector::is_known_pseudo_element_type(pseudo_element)) {
        return;
    }

    ensure_pseudo_element(pseudo_element).layout_node = move(pseudo_element_node);
}

GC::Ptr<Layout::NodeWithStyle> Element::get_pseudo_element_node(CSS::PseudoElement pseudo_element) const
{
    if (auto element_data = get_pseudo_element(pseudo_element); element_data.has_value())
        return element_data->layout_node;
    return nullptr;
}

bool Element::affected_by_hover() const
{
    if (m_computed_properties && m_computed_properties->did_match_any_hover_rules()) {
        return true;
    }
    if (m_pseudo_element_data) {
        for (auto& pseudo_element : *m_pseudo_element_data) {
            if (!pseudo_element.computed_properties)
                continue;
            if (pseudo_element.computed_properties->did_match_any_hover_rules())
                return true;
        }
    }
    return false;
}

// https://html.spec.whatwg.org/multipage/semantics-other.html#selector-enabled
bool Element::matches_enabled_pseudo_class() const
{
    // The :enabled pseudo-class must match any button, input, select, textarea, optgroup, option, fieldset element, or form-associated custom element that is not actually disabled.
    return (is<HTML::HTMLButtonElement>(*this) || is<HTML::HTMLInputElement>(*this) || is<HTML::HTMLSelectElement>(*this) || is<HTML::HTMLTextAreaElement>(*this) || is<HTML::HTMLOptGroupElement>(*this) || is<HTML::HTMLOptionElement>(*this) || is<HTML::HTMLFieldSetElement>(*this))
        && !is_actually_disabled();
}

// https://html.spec.whatwg.org/multipage/semantics-other.html#selector-disabled
bool Element::matches_disabled_pseudo_class() const
{
    // The :disabled pseudo-class must match any element that is actually disabled.
    return is_actually_disabled();
}

// https://html.spec.whatwg.org/multipage/semantics-other.html#selector-checked
bool Element::matches_checked_pseudo_class() const
{
    // The :checked pseudo-class must match any element falling into one of the following categories:
    // - input elements whose type attribute is in the Checkbox state and whose checkedness state is true
    // - input elements whose type attribute is in the Radio Button state and whose checkedness state is true
    if (is<HTML::HTMLInputElement>(*this)) {
        auto const& input_element = static_cast<HTML::HTMLInputElement const&>(*this);
        switch (input_element.type_state()) {
        case HTML::HTMLInputElement::TypeAttributeState::Checkbox:
        case HTML::HTMLInputElement::TypeAttributeState::RadioButton:
            return static_cast<HTML::HTMLInputElement const&>(*this).checked();
        default:
            return false;
        }
    }

    // - option elements whose selectedness is true
    if (is<HTML::HTMLOptionElement>(*this)) {
        return static_cast<HTML::HTMLOptionElement const&>(*this).selected();
    }
    return false;
}

// https://html.spec.whatwg.org/multipage/semantics-other.html#selector-placeholder-shown
bool Element::matches_placeholder_shown_pseudo_class() const
{
    //  The :placeholder-shown pseudo-class must match any element falling into one of the following categories:
    // - input elements that have a placeholder attribute whose value is currently being presented to the user.
    if (is<HTML::HTMLInputElement>(*this) && has_attribute(HTML::AttributeNames::placeholder)) {
        auto const& input_element = static_cast<HTML::HTMLInputElement const&>(*this);
        return input_element.placeholder_element() && input_element.placeholder_value().has_value();
    }
    // - FIXME: textarea elements that have a placeholder attribute whose value is currently being presented to the user.
    return false;
}

// https://html.spec.whatwg.org/multipage/semantics-other.html#selector-link
bool Element::matches_link_pseudo_class() const
{
    // All a elements that have an href attribute, and all area elements that have an href attribute, must match one of :link and :visited.
    if (!is<HTML::HTMLAnchorElement>(*this) && !is<HTML::HTMLAreaElement>(*this) && !is<SVG::SVGAElement>(*this))
        return false;
    return has_attribute(HTML::AttributeNames::href);
}

bool Element::matches_local_link_pseudo_class() const
{
    // The :local-link pseudo-class allows authors to style hyperlinks based on the users current location
    // within a site. It represents an element that is the source anchor of a hyperlink whose target’s
    // absolute URL matches the element’s own document URL. If the hyperlink’s target includes a fragment
    // URL, then the fragment URL of the current URL must also match; if it does not, then the fragment
    // URL portion of the current URL is not taken into account in the comparison.
    if (!matches_link_pseudo_class())
        return false;
    auto document_url = document().url();
    auto maybe_href = attribute(HTML::AttributeNames::href);
    if (!maybe_href.has_value())
        return false;
    auto target_url = document().encoding_parse_url(*maybe_href);
    if (!target_url.has_value())
        return false;
    if (target_url->fragment().has_value())
        return document_url.equals(*target_url, URL::ExcludeFragment::No);
    return document_url.equals(*target_url, URL::ExcludeFragment::Yes);
}

bool Element::includes_properties_from_invalidation_set(CSS::InvalidationSet const& set) const
{
    auto includes_property = [&](CSS::InvalidationSet::Property const& property) {
        switch (property.type) {
        case CSS::InvalidationSet::Property::Type::Class:
            return m_classes.contains_slow(property.name());
        case CSS::InvalidationSet::Property::Type::Id:
            return m_id == property.name();
        case CSS::InvalidationSet::Property::Type::TagName:
            return local_name() == property.name();
        case CSS::InvalidationSet::Property::Type::Attribute: {
            if (property.name() == HTML::AttributeNames::id || property.name() == HTML::AttributeNames::class_)
                return true;
            return has_attribute(property.name());
        }
        case CSS::InvalidationSet::Property::Type::PseudoClass: {
            switch (property.value.get<CSS::PseudoClass>()) {
            case CSS::PseudoClass::Has:
                return true;
            case CSS::PseudoClass::Enabled: {
                return matches_enabled_pseudo_class();
            }
            case CSS::PseudoClass::Disabled: {
                return matches_disabled_pseudo_class();
            }
            case CSS::PseudoClass::Defined: {
                return is_defined();
            }
            case CSS::PseudoClass::Checked: {
                return matches_checked_pseudo_class();
            }
            case CSS::PseudoClass::PlaceholderShown: {
                return matches_placeholder_shown_pseudo_class();
            }
            case CSS::PseudoClass::AnyLink:
            case CSS::PseudoClass::Link:
                return matches_link_pseudo_class();
            case CSS::PseudoClass::LocalLink: {
                return matches_local_link_pseudo_class();
            }
            default:
                VERIFY_NOT_REACHED();
            }
        }
        case CSS::InvalidationSet::Property::Type::InvalidateSelf:
            return false;
        case CSS::InvalidationSet::Property::Type::InvalidateWholeSubtree:
            return true;
        default:
            VERIFY_NOT_REACHED();
        }
    };

    bool includes_any = false;
    set.for_each_property([&](auto const& property) {
        if (includes_property(property)) {
            includes_any = true;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    return includes_any;
}

void Element::invalidate_style_if_affected_by_has()
{
    if (affected_by_has_pseudo_class_in_subject_position()) {
        set_needs_style_update(true);
    }
    if (affected_by_has_pseudo_class_in_non_subject_position()) {
        invalidate_style(StyleInvalidationReason::Other, { { CSS::InvalidationSet::Property::Type::PseudoClass, CSS::PseudoClass::Has } }, {});
    }
}

bool Element::has_pseudo_elements() const
{
    if (m_pseudo_element_data) {
        for (auto& pseudo_element : *m_pseudo_element_data) {
            if (pseudo_element.layout_node)
                return true;
        }
    }
    return false;
}

void Element::clear_pseudo_element_nodes(Badge<Layout::TreeBuilder>)
{
    if (m_pseudo_element_data) {
        for (auto& pseudo_element : *m_pseudo_element_data) {
            pseudo_element.layout_node = nullptr;
        }
    }
}

void Element::serialize_pseudo_elements_as_json(JsonArraySerializer<StringBuilder>& children_array) const
{
    if (!m_pseudo_element_data)
        return;
    for (size_t i = 0; i < m_pseudo_element_data->size(); ++i) {
        auto& pseudo_element = (*m_pseudo_element_data)[i].layout_node;
        if (!pseudo_element)
            continue;
        auto object = MUST(children_array.add_object());
        MUST(object.add("name"sv, MUST(String::formatted("::{}", CSS::pseudo_element_name(static_cast<CSS::PseudoElement>(i))))));
        MUST(object.add("type"sv, "pseudo-element"));
        MUST(object.add("parent-id"sv, unique_id().value()));
        MUST(object.add("pseudo-element"sv, i));
        MUST(object.finish());
    }
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-tabindex
i32 Element::default_tab_index_value() const
{
    // The default value is 0 if the element is an a, area, button, frame, iframe, input, object, select, textarea, or SVG a element, or is a summary element that is a summary for its parent details.
    // The default value is −1 otherwise.
    // Note: The varying default value based on element type is a historical artifact.
    return -1;
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-tabindex
i32 Element::tab_index() const
{
    auto maybe_table_index = Web::HTML::parse_integer(get_attribute_value(HTML::AttributeNames::tabindex));

    if (!maybe_table_index.has_value())
        return default_tab_index_value();
    return maybe_table_index.value();
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-tabindex
void Element::set_tab_index(i32 tab_index)
{
    MUST(set_attribute(HTML::AttributeNames::tabindex, String::number(tab_index)));
}

// https://drafts.csswg.org/cssom-view/#potentially-scrollable
bool Element::is_potentially_scrollable() const
{
    // NOTE: Ensure that layout is up-to-date before looking at metrics.
    const_cast<Document&>(document()).update_layout(UpdateLayoutReason::ElementIsPotentiallyScrollable);

    // An element body (which will be the body element) is potentially scrollable if all of the following conditions are true:
    VERIFY(is<HTML::HTMLBodyElement>(this) || is<HTML::HTMLFrameSetElement>(this));

    // Since this should always be the body element, the body element must have a <html> element parent. See Document::body().
    VERIFY(parent());

    // - body has an associated box.
    // - body’s parent element’s computed value of the overflow-x or overflow-y properties is neither visible nor clip.
    // - body’s computed value of the overflow-x or overflow-y properties is neither visible nor clip.
    return layout_node()
        && (parent()->layout_node()
            && parent()->layout_node()->computed_values().overflow_x() != CSS::Overflow::Visible && parent()->layout_node()->computed_values().overflow_x() != CSS::Overflow::Clip
            && parent()->layout_node()->computed_values().overflow_y() != CSS::Overflow::Visible && parent()->layout_node()->computed_values().overflow_y() != CSS::Overflow::Clip)
        && (layout_node()->computed_values().overflow_x() != CSS::Overflow::Visible && layout_node()->computed_values().overflow_x() != CSS::Overflow::Clip
            && layout_node()->computed_values().overflow_y() != CSS::Overflow::Visible && layout_node()->computed_values().overflow_y() != CSS::Overflow::Clip);
}

// https://drafts.csswg.org/cssom-view/#dom-element-scrolltop
double Element::scroll_top() const
{
    // 1. Let document be the element’s node document.
    auto& document = this->document();

    // 2. If document is not the active document, return zero and terminate these steps.
    if (!document.is_active())
        return 0.0;

    // 3. Let window be the value of document’s defaultView attribute.
    // FIXME: The specification expects defaultView to be a Window object, but defaultView actually returns a WindowProxy object.
    auto window = document.window();

    // 4. If window is null, return zero and terminate these steps.
    if (!window)
        return 0.0;

    // 5. If the element is the root element and document is in quirks mode, return zero and terminate these steps.
    if (document.document_element() == this && document.in_quirks_mode())
        return 0.0;

    // 6. If the element is the root element return the value of scrollY on window.
    if (document.document_element() == this)
        return window->scroll_y();

    // NOTE: Ensure that layout is up-to-date before looking at metrics.
    const_cast<Document&>(document).update_layout(UpdateLayoutReason::ElementScrollTop);

    // 7. If the element is the body element, document is in quirks mode, and the element is not potentially scrollable, return the value of scrollY on window.
    if (document.body() == this && document.in_quirks_mode() && !is_potentially_scrollable())
        return window->scroll_y();

    // 8. If the element does not have any associated box, return zero and terminate these steps.
    if (!paintable_box())
        return 0.0;

    // 9. Return the y-coordinate of the scrolling area at the alignment point with the top of the padding edge of the element.
    // FIXME: Is this correct?
    return paintable_box()->scroll_offset().y().to_double();
}

// https://drafts.csswg.org/cssom-view/#dom-element-scrollleft
double Element::scroll_left() const
{
    // 1. Let document be the element’s node document.
    auto& document = this->document();

    // 2. If document is not the active document, return zero and terminate these steps.
    if (!document.is_active())
        return 0.0;

    // 3. Let window be the value of document’s defaultView attribute.
    // FIXME: The specification expects defaultView to be a Window object, but defaultView actually returns a WindowProxy object.
    auto window = document.window();

    // 4. If window is null, return zero and terminate these steps.
    if (!window)
        return 0.0;

    // 5. If the element is the root element and document is in quirks mode, return zero and terminate these steps.
    if (document.document_element() == this && document.in_quirks_mode())
        return 0.0;

    // 6. If the element is the root element return the value of scrollX on window.
    if (document.document_element() == this)
        return window->scroll_x();

    // NOTE: Ensure that layout is up-to-date before looking at metrics.
    const_cast<Document&>(document).update_layout(UpdateLayoutReason::ElementScrollLeft);

    // 7. If the element is the body element, document is in quirks mode, and the element is not potentially scrollable, return the value of scrollX on window.
    if (document.body() == this && document.in_quirks_mode() && !is_potentially_scrollable())
        return window->scroll_x();

    // 8. If the element does not have any associated box, return zero and terminate these steps.
    if (!paintable_box())
        return 0.0;

    // 9. Return the x-coordinate of the scrolling area at the alignment point with the left of the padding edge of the element.
    // FIXME: Is this correct?
    return paintable_box()->scroll_offset().x().to_double();
}

// https://drafts.csswg.org/cssom-view/#dom-element-scrollleft
void Element::set_scroll_left(double x)
{
    // 1. Let x be the given value.

    // 2. Normalize non-finite values for x.
    x = HTML::normalize_non_finite_values(x);

    // 3. Let document be the element’s node document.
    auto& document = this->document();

    // 4. If document is not the active document, terminate these steps.
    if (!document.is_active())
        return;

    // 5. Let window be the value of document’s defaultView attribute.
    // FIXME: The specification expects defaultView to be a Window object, but defaultView actually returns a WindowProxy object.
    auto window = document.window();

    // 6. If window is null, terminate these steps.
    if (!window)
        return;

    // 7. If the element is the root element and document is in quirks mode, terminate these steps.
    if (document.document_element() == this && document.in_quirks_mode())
        return;

    // 8. If the element is the root element invoke scroll() on window with x as first argument and scrollY on window as second argument, and terminate these steps.
    if (document.document_element() == this) {
        window->scroll(x, window->scroll_y());
        return;
    }

    // NOTE: Ensure that layout is up-to-date before looking at metrics or scrolling the page.
    const_cast<Document&>(document).update_layout(UpdateLayoutReason::ElementSetScrollLeft);

    // 9. If the element is the body element, document is in quirks mode, and the element is not potentially scrollable, invoke scroll() on window with x as first argument and scrollY on window as second argument, and terminate these steps.
    if (document.body() == this && document.in_quirks_mode() && !is_potentially_scrollable()) {
        window->scroll(x, window->scroll_y());
        return;
    }

    // 10. If the element does not have any associated box, the element has no associated scrolling box, or the element has no overflow, terminate these steps.
    if (!paintable_box())
        return;

    if (!paintable_box()->layout_node_with_style_and_box_metrics().is_scroll_container())
        return;

    // FIXME: or the element has no overflow.

    // 11. Scroll the element to x,scrollTop, with the scroll behavior being "auto".
    // FIXME: Implement this in terms of calling "scroll the element".
    auto scroll_offset = paintable_box()->scroll_offset();
    scroll_offset.set_x(CSSPixels::nearest_value_for(x));
    paintable_box()->set_scroll_offset(scroll_offset);
}

void Element::set_scroll_top(double y)
{
    // 1. Let y be the given value.

    // 2. Normalize non-finite values for y.
    y = HTML::normalize_non_finite_values(y);

    // 3. Let document be the element’s node document.
    auto& document = this->document();

    // 4. If document is not the active document, terminate these steps.
    if (!document.is_active())
        return;

    // 5. Let window be the value of document’s defaultView attribute.
    // FIXME: The specification expects defaultView to be a Window object, but defaultView actually returns a WindowProxy object.
    auto window = document.window();

    // 6. If window is null, terminate these steps.
    if (!window)
        return;

    // 7. If the element is the root element and document is in quirks mode, terminate these steps.
    if (document.document_element() == this && document.in_quirks_mode())
        return;

    // 8. If the element is the root element invoke scroll() on window with scrollX on window as first argument and y as second argument, and terminate these steps.
    if (document.document_element() == this) {
        window->scroll(window->scroll_x(), y);
        return;
    }

    // NOTE: Ensure that layout is up-to-date before looking at metrics or scrolling the page.
    const_cast<Document&>(document).update_layout(UpdateLayoutReason::ElementSetScrollTop);

    // 9. If the element is the body element, document is in quirks mode, and the element is not potentially scrollable, invoke scroll() on window with scrollX as first argument and y as second argument, and terminate these steps.
    if (document.body() == this && document.in_quirks_mode() && !is_potentially_scrollable()) {
        window->scroll(window->scroll_x(), y);
        return;
    }

    // 10. If the element does not have any associated box, the element has no associated scrolling box, or the element has no overflow, terminate these steps.
    if (!paintable_box())
        return;

    if (!paintable_box()->layout_node_with_style_and_box_metrics().is_scroll_container())
        return;

    // FIXME: or the element has no overflow.

    // 11. Scroll the element to scrollLeft,y, with the scroll behavior being "auto".
    // FIXME: Implement this in terms of calling "scroll the element".
    auto scroll_offset = paintable_box()->scroll_offset();
    scroll_offset.set_y(CSSPixels::nearest_value_for(y));
    paintable_box()->set_scroll_offset(scroll_offset);
}

// https://drafts.csswg.org/cssom-view/#dom-element-scrollwidth
int Element::scroll_width() const
{
    // 1. Let document be the element’s node document.
    auto& document = this->document();

    // 2. If document is not the active document, return zero and terminate these steps.
    if (!document.is_active())
        return 0;

    // 3. Let viewport width be the width of the viewport excluding the width of the scroll bar, if any,
    //    or zero if there is no viewport.
    auto viewport_width = document.viewport_rect().width().to_int();
    auto viewport_scroll_width = document.navigable()->size().width().to_int();

    // 4. If the element is the root element and document is not in quirks mode
    //    return max(viewport scrolling area width, viewport width).
    if (document.document_element() == this && !document.in_quirks_mode())
        return max(viewport_scroll_width, viewport_width);

    // NOTE: Ensure that layout is up-to-date before looking at metrics.
    const_cast<Document&>(document).update_layout(UpdateLayoutReason::ElementScrollWidth);

    // 5. If the element is the body element, document is in quirks mode and the element is not potentially scrollable,
    //    return max(viewport scrolling area width, viewport width).
    if (document.body() == this && document.in_quirks_mode() && !is_potentially_scrollable())
        return max(viewport_scroll_width, viewport_width);

    // 6. If the element does not have any associated box return zero and terminate these steps.
    if (!paintable_box())
        return 0;

    // 7. Return the width of the element’s scrolling area.
    if (auto scrollable_overflow_rect = paintable_box()->scrollable_overflow_rect(); scrollable_overflow_rect.has_value()) {
        return scrollable_overflow_rect->width().to_int();
    }
    return 0;
}

// https://drafts.csswg.org/cssom-view/#dom-element-scrollheight
int Element::scroll_height() const
{
    // 1. Let document be the element’s node document.
    auto& document = this->document();

    // 2. If document is not the active document, return zero and terminate these steps.
    if (!document.is_active())
        return 0;

    // 3. Let viewport height be the height of the viewport excluding the height of the scroll bar, if any,
    //    or zero if there is no viewport.
    auto viewport_height = document.viewport_rect().height().to_int();
    auto viewport_scroll_height = document.navigable()->size().height().to_int();

    // 4. If the element is the root element and document is not in quirks mode
    //    return max(viewport scrolling area height, viewport height).
    if (document.document_element() == this && !document.in_quirks_mode())
        return max(viewport_scroll_height, viewport_height);

    // NOTE: Ensure that layout is up-to-date before looking at metrics.
    const_cast<Document&>(document).update_layout(UpdateLayoutReason::ElementScrollHeight);

    // 5. If the element is the body element, document is in quirks mode and the element is not potentially scrollable,
    //    return max(viewport scrolling area height, viewport height).
    if (document.body() == this && document.in_quirks_mode() && !is_potentially_scrollable())
        return max(viewport_scroll_height, viewport_height);

    // 6. If the element does not have any associated box return zero and terminate these steps.
    if (!paintable_box())
        return 0;

    // 7. Return the height of the element’s scrolling area.
    if (auto scrollable_overflow_rect = paintable_box()->scrollable_overflow_rect(); scrollable_overflow_rect.has_value()) {
        return scrollable_overflow_rect->height().to_int();
    }
    return 0;
}

// https://html.spec.whatwg.org/multipage/semantics-other.html#concept-element-disabled
bool Element::is_actually_disabled() const
{
    // An element is said to be actually disabled if it is one of the following:
    // - a button element that is disabled
    // - an input element that is disabled
    // - a select element that is disabled
    // - a textarea element that is disabled
    if (is<HTML::HTMLButtonElement>(this) || is<HTML::HTMLInputElement>(this) || is<HTML::HTMLSelectElement>(this) || is<HTML::HTMLTextAreaElement>(this)) {
        auto const* form_associated_element = dynamic_cast<HTML::FormAssociatedElement const*>(this);
        VERIFY(form_associated_element);

        return !form_associated_element->enabled();
    }

    // - an optgroup element that has a disabled attribute
    if (is<HTML::HTMLOptGroupElement>(this))
        return has_attribute(HTML::AttributeNames::disabled);

    // - an option element that is disabled
    if (is<HTML::HTMLOptionElement>(this))
        return static_cast<HTML::HTMLOptionElement const&>(*this).disabled();

    // - a fieldset element that is a disabled fieldset
    if (is<HTML::HTMLFieldSetElement>(this))
        return static_cast<HTML::HTMLFieldSetElement const&>(*this).is_disabled();

    // FIXME: - a form-associated custom element that is disabled
    return false;
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#fragment-parsing-algorithm-steps
WebIDL::ExceptionOr<GC::Ref<DOM::DocumentFragment>> Element::parse_fragment(StringView markup)
{
    // 1. Let algorithm be the HTML fragment parsing algorithm.
    auto algorithm = HTML::HTMLParser::parse_html_fragment;

    // FIXME: 2. If context's node document is an XML document, then set algorithm to the XML fragment parsing algorithm.
    if (document().is_xml_document()) {
        dbgln("FIXME: Handle fragment parsing of XML documents");
    }

    // 3. Let newChildren be the result of invoking algorithm given context and markup.
    auto new_children = algorithm(*this, markup, HTML::HTMLParser::AllowDeclarativeShadowRoots::No);

    // 4. Let fragment be a new DocumentFragment whose node document is context's node document.
    auto fragment = realm().create<DOM::DocumentFragment>(document());

    // 5. For each node of newChildren, in tree order: append node to fragment.
    for (auto& child : new_children)
        TRY(fragment->append_child(*child));

    // 6. Return fragment.
    return fragment;
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-element-outerhtml
WebIDL::ExceptionOr<String> Element::outer_html() const
{
    return serialize_fragment(HTML::RequireWellFormed::Yes, FragmentSerializationMode::Outer);
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-element-outerhtml
WebIDL::ExceptionOr<void> Element::set_outer_html(String const& value)
{
    // 1. FIXME: Let compliantString be the result of invoking the Get Trusted Type compliant string algorithm with TrustedHTML, this's relevant global object, the given value, "Element outerHTML", and "script".

    // 2. Let parent be this's parent.
    auto* parent = this->parent();

    // 3. If parent is null, return. There would be no way to obtain a reference to the nodes created even if the remaining steps were run.
    if (!parent)
        return {};

    // 4. If parent is a Document, throw a "NoModificationAllowedError" DOMException.
    if (parent->is_document())
        return WebIDL::NoModificationAllowedError::create(realm(), "Cannot set outer HTML on document"_string);

    // 5. If parent is a DocumentFragment, set parent to the result of creating an element given this's node document, "body", and the HTML namespace.
    if (parent->is_document_fragment())
        parent = TRY(create_element(document(), HTML::TagNames::body, Namespace::HTML));

    // 6. Let fragment be the result of invoking the fragment parsing algorithm steps given parent and compliantString. FIXME: Use compliantString.
    auto fragment = TRY(as<Element>(*parent).parse_fragment(value));

    // 6. Replace this with fragment within this's parent.
    TRY(parent->replace_child(fragment, *this));

    return {};
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#the-insertadjacenthtml()-method
WebIDL::ExceptionOr<void> Element::insert_adjacent_html(String const& position, String const& string)
{
    // 1. Let context be null.
    GC::Ptr<Node> context;

    // 2. Use the first matching item from this list:
    // - If position is an ASCII case-insensitive match for the string "beforebegin"
    // - If position is an ASCII case-insensitive match for the string "afterend"
    if (Infra::is_ascii_case_insensitive_match(position, "beforebegin"sv)
        || Infra::is_ascii_case_insensitive_match(position, "afterend"sv)) {
        // 1. Set context to this's parent.
        context = this->parent();

        // 2. If context is null or a Document, throw a "NoModificationAllowedError" DOMException.
        if (!context || context->is_document())
            return WebIDL::NoModificationAllowedError::create(realm(), "insertAdjacentHTML: context is null or a Document"_string);
    }
    // - If position is an ASCII case-insensitive match for the string "afterbegin"
    // - If position is an ASCII case-insensitive match for the string "beforeend"
    else if (Infra::is_ascii_case_insensitive_match(position, "afterbegin"sv)
        || Infra::is_ascii_case_insensitive_match(position, "beforeend"sv)) {
        // Set context to this.
        context = this;
    }
    // Otherwise
    else {
        // Throw a "SyntaxError" DOMException.
        return WebIDL::SyntaxError::create(realm(), "insertAdjacentHTML: invalid position argument"_string);
    }

    // 3. If context is not an Element or the following are all true:
    //    - context's node document is an HTML document,
    //    - context's local name is "html", and
    //    - context's namespace is the HTML namespace;
    if (!is<Element>(*context)
        || (context->document().document_type() == Document::Type::HTML
            && static_cast<Element const&>(*context).local_name() == "html"sv
            && static_cast<Element const&>(*context).namespace_uri() == Namespace::HTML)) {
        context = TRY(create_element(document(), HTML::TagNames::body, Namespace::HTML));
    }

    // 4. Let fragment be the result of invoking the fragment parsing algorithm steps with context and string.
    auto fragment = TRY(as<Element>(*context).parse_fragment(string));

    // 5. Use the first matching item from this list:

    // - If position is an ASCII case-insensitive match for the string "beforebegin"
    if (Infra::is_ascii_case_insensitive_match(position, "beforebegin"sv)) {
        // Insert fragment into this's parent before this.
        parent()->insert_before(fragment, this);
    }

    // - If position is an ASCII case-insensitive match for the string "afterbegin"
    else if (Infra::is_ascii_case_insensitive_match(position, "afterbegin"sv)) {
        // Insert fragment into this before its first child.
        insert_before(fragment, first_child());
    }

    // - If position is an ASCII case-insensitive match for the string "beforeend"
    else if (Infra::is_ascii_case_insensitive_match(position, "beforeend"sv)) {
        // Append fragment to this.
        TRY(append_child(fragment));
    }

    // - If position is an ASCII case-insensitive match for the string "afterend"
    else if (Infra::is_ascii_case_insensitive_match(position, "afterend"sv)) {
        // Insert fragment into this's parent before this's next sibling.
        parent()->insert_before(fragment, next_sibling());
    }
    return {};
}

// https://dom.spec.whatwg.org/#insert-adjacent
WebIDL::ExceptionOr<GC::Ptr<Node>> Element::insert_adjacent(StringView where, GC::Ref<Node> node)
{
    // To insert adjacent, given an element element, string where, and a node node, run the steps associated with the first ASCII case-insensitive match for where:
    if (Infra::is_ascii_case_insensitive_match(where, "beforebegin"sv)) {
        // -> "beforebegin"
        // If element’s parent is null, return null.
        if (!parent())
            return GC::Ptr<Node> { nullptr };

        // Return the result of pre-inserting node into element’s parent before element.
        return GC::Ptr<Node> { TRY(parent()->pre_insert(move(node), this)) };
    }

    if (Infra::is_ascii_case_insensitive_match(where, "afterbegin"sv)) {
        // -> "afterbegin"
        // Return the result of pre-inserting node into element before element’s first child.
        return GC::Ptr<Node> { TRY(pre_insert(move(node), first_child())) };
    }

    if (Infra::is_ascii_case_insensitive_match(where, "beforeend"sv)) {
        // -> "beforeend"
        // Return the result of pre-inserting node into element before null.
        return GC::Ptr<Node> { TRY(pre_insert(move(node), nullptr)) };
    }

    if (Infra::is_ascii_case_insensitive_match(where, "afterend"sv)) {
        // -> "afterend"
        // If element’s parent is null, return null.
        if (!parent())
            return GC::Ptr<Node> { nullptr };

        // Return the result of pre-inserting node into element’s parent before element’s next sibling.
        return GC::Ptr<Node> { TRY(parent()->pre_insert(move(node), next_sibling())) };
    }

    // -> Otherwise
    // Throw a "SyntaxError" DOMException.
    return WebIDL::SyntaxError::create(realm(), MUST(String::formatted("Unknown position '{}'. Must be one of 'beforebegin', 'afterbegin', 'beforeend' or 'afterend'"sv, where)));
}

// https://dom.spec.whatwg.org/#dom-element-insertadjacentelement
WebIDL::ExceptionOr<GC::Ptr<Element>> Element::insert_adjacent_element(String const& where, GC::Ref<Element> element)
{
    // The insertAdjacentElement(where, element) method steps are to return the result of running insert adjacent, give this, where, and element.
    auto returned_node = TRY(insert_adjacent(where, element));
    if (!returned_node)
        return GC::Ptr<Element> { nullptr };
    return GC::Ptr<Element> { as<Element>(*returned_node) };
}

// https://dom.spec.whatwg.org/#dom-element-insertadjacenttext
WebIDL::ExceptionOr<void> Element::insert_adjacent_text(String const& where, String const& data)
{
    // 1. Let text be a new Text node whose data is data and node document is this’s node document.
    auto text = realm().create<DOM::Text>(document(), data);

    // 2. Run insert adjacent, given this, where, and text.
    // Spec Note: This method returns nothing because it existed before we had a chance to design it.
    (void)TRY(insert_adjacent(where, text));
    return {};
}

// https://drafts.csswg.org/cssom-view-1/#determine-the-scroll-into-view-position
static CSSPixelPoint determine_the_scroll_into_view_position(Element& target, Bindings::ScrollLogicalPosition block, Bindings::ScrollLogicalPosition inline_, Node& scrolling_box)
{
    // To determine the scroll-into-view position of a target, which is an Element, pseudo-element, or Range, with a
    // block flow direction position block, an inline base direction position inline, and a scrolling box scrolling box,
    // run the following steps:

    if (!scrolling_box.is_document()) {
        // FIXME: Add support for scrolling boxes other than the viewport.
        return {};
    }
    // NOTE: For a viewport scrolling box is initial containing block
    CSSPixelRect scrolling_box_rect = scrolling_box.document().viewport_rect();

    // FIXME: All of this needs to support different block/inline directions.

    // 1. Let target bounding border box be the box represented by the return value of invoking Element’s
    //    getBoundingClientRect(), if target is an Element, or Range’s getBoundingClientRect(),
    //    if target is a Range.
    auto target_bounding_border_box = target.get_bounding_client_rect();

    // 2. Let scrolling box edge A be the beginning edge in the block flow direction of scrolling box, and
    //    let element edge A be target bounding border box’s edge on the same physical side as that of
    //    scrolling box edge A.
    CSSPixels element_edge_a = target_bounding_border_box.top();
    CSSPixels scrolling_box_edge_a = scrolling_box_rect.top();

    // 3. Let scrolling box edge B be the ending edge in the block flow direction of scrolling box, and let
    //    element edge B be target bounding border box’s edge on the same physical side as that of scrolling
    //    box edge B.
    CSSPixels element_edge_b = target_bounding_border_box.bottom();
    CSSPixels scrolling_box_edge_b = scrolling_box_rect.bottom();

    // 4. Let scrolling box edge C be the beginning edge in the inline base direction of scrolling box, and
    //    let element edge C be target bounding border box’s edge on the same physical side as that of scrolling
    //    box edge C.
    CSSPixels element_edge_c = target_bounding_border_box.left();
    CSSPixels scrolling_box_edge_c = scrolling_box_rect.left();

    // 5. Let scrolling box edge D be the ending edge in the inline base direction of scrolling box, and let element
    //    edge D be target bounding border box’s edge on the same physical side as that of scrolling box edge D.
    CSSPixels element_edge_d = target_bounding_border_box.right();
    CSSPixels scrolling_box_edge_d = scrolling_box_rect.right();

    // 6. Let element height be the distance between element edge A and element edge B.
    CSSPixels element_height = element_edge_b - element_edge_a;

    // 7. Let scrolling box height be the distance between scrolling box edge A and scrolling box edge B.
    CSSPixels scrolling_box_height = scrolling_box_edge_b - scrolling_box_edge_a;

    // 8. Let element width be the distance between element edge C and element edge D.
    CSSPixels element_width = element_edge_d - element_edge_c;

    // 9. Let scrolling box width be the distance between scrolling box edge C and scrolling box edge D.
    CSSPixels scrolling_box_width = scrolling_box_edge_d - scrolling_box_edge_c;

    // 10. Let position be the scroll position scrolling box would have by following these steps:
    auto position = [&]() -> CSSPixelPoint {
        CSSPixels x = 0;
        CSSPixels y = 0;

        // 1. If block is "start", then align element edge A with scrolling box edge A.
        if (block == Bindings::ScrollLogicalPosition::Start) {
            y = element_edge_a;
        }
        // 2. Otherwise, if block is "end", then align element edge B with scrolling box edge B.
        else if (block == Bindings::ScrollLogicalPosition::End) {
            y = element_edge_a + element_height - scrolling_box_height;
        }
        // 3. Otherwise, if block is "center", then align the center of target bounding border box with the center of
        //    scrolling box in scrolling box’s block flow direction.
        else if (block == Bindings::ScrollLogicalPosition::Center) {
            y = element_edge_a + (element_height / 2) - (scrolling_box_height / 2);
        }
        // 4. Otherwise, block is "nearest":
        else {
            // If element edge A and element edge B are both outside scrolling box edge A and scrolling box edge B
            if (element_edge_a <= 0 && element_edge_b >= scrolling_box_height) {
                // Do nothing.
            }
            // If element edge A is outside scrolling box edge A and element height is less than scrolling box height
            // If element edge B is outside scrolling box edge B and element height is greater than scrolling box height
            else if ((element_edge_a <= 0 && element_height < scrolling_box_height) || (element_edge_b >= scrolling_box_height && element_height > scrolling_box_height)) {
                // Align element edge A with scrolling box edge A.
                y = element_edge_a;
            }
            // If element edge A is outside scrolling box edge A and element height is greater than scrolling box height
            // If element edge B is outside scrolling box edge B and element height is less than scrolling box height
            else if ((element_edge_b >= scrolling_box_height && element_height < scrolling_box_height) || (element_edge_a <= 0 && element_height > scrolling_box_height)) {
                // Align element edge B with scrolling box edge B.
                y = element_edge_a + element_height - scrolling_box_height;
            }
        }

        // 5. If inline is "start", then align element edge C with scrolling box edge C.
        if (inline_ == Bindings::ScrollLogicalPosition::Start) {
            x = element_edge_c;
        }
        // 6. Otherwise, if inline is "end", then align element edge D with scrolling box edge D.
        else if (inline_ == Bindings::ScrollLogicalPosition::End) {
            x = element_edge_d + element_width - scrolling_box_width;
        }
        // 7. Otherwise, if inline is "center", then align the center of target bounding border box with the center of
        //    scrolling box in scrolling box’s inline base direction.
        else if (inline_ == Bindings::ScrollLogicalPosition::Center) {
            x = element_edge_c + (element_width / 2) - (scrolling_box_width / 2);
        }
        // 8. Otherwise, inline is "nearest":
        else {
            // If element edge C and element edge D are both outside scrolling box edge C and scrolling box edge D
            if (element_edge_c <= 0 && element_edge_d >= scrolling_box_width) {
                // Do nothing.
            }
            // If element edge C is outside scrolling box edge C and element width is less than scrolling box width
            // If element edge D is outside scrolling box edge D and element width is greater than scrolling box width
            else if ((element_edge_c <= 0 && element_width < scrolling_box_width) || (element_edge_d >= scrolling_box_width && element_width > scrolling_box_width)) {
                // Align element edge C with scrolling box edge C.
                x = element_edge_c;
            }
            // If element edge C is outside scrolling box edge C and element width is greater than scrolling box width
            // If element edge D is outside scrolling box edge D and element width is less than scrolling box width
            else if ((element_edge_d >= scrolling_box_width && element_width < scrolling_box_width) || (element_edge_c <= 0 && element_width > scrolling_box_width)) {
                // Align element edge D with scrolling box edge D.
                x = element_edge_d + element_width - scrolling_box_width;
            }
        }

        return CSSPixelPoint { x, y };
    }();

    // 11. Return position.
    return position;
}

// https://drafts.csswg.org/cssom-view-1/#scroll-a-target-into-view
static ErrorOr<void> scroll_an_element_into_view(Element& target, Bindings::ScrollBehavior behavior, Bindings::ScrollLogicalPosition block, Bindings::ScrollLogicalPosition inline_, GC::Ptr<Element> container)
{
    // To scroll a target into view target, which is an Element, pseudo-element, or Range, with a scroll behavior behavior,
    // a block flow direction position block, an inline base direction position inline, and an optional containing Element
    // to stop scrolling after reaching container, means to run these steps:

    // 1. For each ancestor element or viewport that establishes a scrolling box scrolling box, in order of innermost
    //    to outermost scrolling box, run these substeps:
    auto* ancestor = target.parent();
    Vector<Node&> scrolling_boxes;
    while (ancestor) {
        if (ancestor->paintable_box() && ancestor->paintable_box()->has_scrollable_overflow())
            scrolling_boxes.append(*ancestor);
        ancestor = ancestor->parent();
    }

    for (auto& scrolling_box : scrolling_boxes) {
        // 1. If the Document associated with target is not same origin with the Document
        //    associated with the element or viewport associated with scrolling box, terminate these steps.
        if (target.document().origin() != scrolling_box.document().origin()) {
            break;
        }

        // NOTE: For a viewport scrolling box is initial containing block
        // CSSPixelRect scrolling_box = scrolling_box.document().viewport_rect();

        // 2. Let position be the scroll position resulting from running the steps to determine the scroll-into-view
        //    position of target with block as the block flow position, inline as the inline base direction position
        //    and scrolling box as the scrolling box.
        auto position = determine_the_scroll_into_view_position(target, block, inline_, scrolling_box);

        // 3. If position is not the same as scrolling box’s current scroll position, or scrolling box has an ongoing smooth scroll,
        // FIXME: Actually check this condition.
        if (true) {
            // -> If scrolling box is associated with an element
            if (scrolling_box.is_element()) {
                // FIXME: Perform a scroll of the element’s scrolling box to position, with the element as the associated element and behavior as the scroll behavior.
            }
            // -> If scrolling box is associated with a viewport
            else if (scrolling_box.is_document()) {
                // 1. Let document be the viewport’s associated Document.
                auto& document = static_cast<Document&>(scrolling_box);

                // FIXME: 2. Let root element be document’s root element, if there is one, or null otherwise.
                // FIXME: 3. Perform a scroll of the viewport to position, with root element as the associated element and behavior as the scroll behavior.
                (void)behavior;

                // AD-HOC:
                // NOTE: Since calculated position is relative to the viewport, we need to add the viewport's position to it
                //       before passing to perform_scroll_of_viewport() that expects a position relative to the page.
                position.set_y(position.y() + document.viewport_rect().y());
                document.navigable()->perform_scroll_of_viewport(position);
            }
        }

        // 4. If container is not null and scrolling box is a shadow-including inclusive ancestor of container,
        //    abort the rest of these steps.
        if (container != nullptr && scrolling_box.is_shadow_including_ancestor_of(*container))
            break;
    }

    return {};
}

// https://w3c.github.io/csswg-drafts/cssom-view-1/#dom-element-scrollintoview
ErrorOr<void> Element::scroll_into_view(Optional<Variant<bool, ScrollIntoViewOptions>> arg)
{
    // 1. Let behavior be "auto".
    auto behavior = Bindings::ScrollBehavior::Auto;

    // 2. Let block be "start".
    auto block = Bindings::ScrollLogicalPosition::Start;

    // 3. Let inline be "nearest".
    auto inline_ = Bindings::ScrollLogicalPosition::Nearest;

    // 4. Let container be null.
    GC::Ptr<Element> container = nullptr;

    // 5. If arg is a ScrollIntoViewOptions dictionary, then:
    if (arg.has_value() && arg->has<ScrollIntoViewOptions>()) {
        auto options = arg->get<ScrollIntoViewOptions>();

        // 1. Set behavior to the behavior dictionary member of options.
        behavior = options.behavior;

        // 2. Set block to the block dictionary member of options.
        block = options.block;

        // 3. Set inline to the inline dictionary member of options.
        inline_ = options.inline_;

        // 4. If the container dictionary member of options is "nearest", set container to this element.
        if (options.container == Bindings::ScrollIntoViewContainer::Nearest)
            container = this;
    }
    // 6. Otherwise, if arg is false, then set block to "end".
    else if (arg.has_value() && arg->has<bool>() && arg->get<bool>() == false) {
        block = Bindings::ScrollLogicalPosition::End;
    }

    // 7. If the element does not have any associated box, or is not available to user-agent features, then return.
    document().update_layout(UpdateLayoutReason::ElementScrollIntoView);
    if (!layout_node())
        return Error::from_string_literal("Element has no associated box");

    // 8. Scroll the element into view with behavior, block, and inline.
    TRY(scroll_an_element_into_view(*this, behavior, block, inline_, container));

    // FIXME: 9. Optionally perform some other action that brings the element to the user’s attention.

    return {};
}

void Element::invalidate_style_after_attribute_change(FlyString const& attribute_name, Optional<String> const& old_value, Optional<String> const& new_value)
{
    Vector<CSS::InvalidationSet::Property, 1> changed_properties;
    StyleInvalidationOptions style_invalidation_options;
    if (is_presentational_hint(attribute_name)) {
        style_invalidation_options.invalidate_self = true;
    }

    if (style_uses_css_custom_properties()) {
        // A css custom property can be hooked on to this element by any attribute
        // so invalidate elements and rerender them in that scenario
        style_invalidation_options.invalidate_elements_that_use_css_custom_properties = true;
    }

    if (attribute_name == HTML::AttributeNames::style) {
        style_invalidation_options.invalidate_self = true;
        // even if we don't have custom properties, the new "style" attribute could add one
        style_invalidation_options.invalidate_elements_that_use_css_custom_properties = true;
    } else if (attribute_name == HTML::AttributeNames::class_) {
        // adding or removing classes can add new custom properties to this element
        style_invalidation_options.invalidate_elements_that_use_css_custom_properties = true;

        Vector<StringView> old_classes;
        Vector<StringView> new_classes;
        if (old_value.has_value())
            old_classes = old_value->bytes_as_string_view().split_view_if(Infra::is_ascii_whitespace);
        if (new_value.has_value())
            new_classes = new_value->bytes_as_string_view().split_view_if(Infra::is_ascii_whitespace);
        for (auto& old_class : old_classes) {
            if (!new_classes.contains_slow(old_class)) {
                changed_properties.append({ .type = CSS::InvalidationSet::Property::Type::Class, .value = FlyString::from_utf8_without_validation(old_class.bytes()) });
            }
        }
        for (auto& new_class : new_classes) {
            if (!old_classes.contains_slow(new_class)) {
                changed_properties.append({ .type = CSS::InvalidationSet::Property::Type::Class, .value = FlyString::from_utf8_without_validation(new_class.bytes()) });
            }
        }
    } else if (attribute_name == HTML::AttributeNames::id) {
        if (old_value.has_value())
            changed_properties.append({ .type = CSS::InvalidationSet::Property::Type::Id, .value = FlyString(old_value.value()) });
        if (new_value.has_value())
            changed_properties.append({ .type = CSS::InvalidationSet::Property::Type::Id, .value = FlyString(new_value.value()) });
    } else if (attribute_name == HTML::AttributeNames::disabled) {
        changed_properties.append({ .type = CSS::InvalidationSet::Property::Type::PseudoClass, .value = CSS::PseudoClass::Disabled });
        changed_properties.append({ .type = CSS::InvalidationSet::Property::Type::PseudoClass, .value = CSS::PseudoClass::Enabled });
    } else if (attribute_name == HTML::AttributeNames::placeholder) {
        changed_properties.append({ .type = CSS::InvalidationSet::Property::Type::PseudoClass, .value = CSS::PseudoClass::PlaceholderShown });
    } else if (attribute_name == HTML::AttributeNames::value) {
        changed_properties.append({ .type = CSS::InvalidationSet::Property::Type::PseudoClass, .value = CSS::PseudoClass::Checked });
    }

    changed_properties.append({ .type = CSS::InvalidationSet::Property::Type::Attribute, .value = attribute_name });
    invalidate_style(StyleInvalidationReason::ElementAttributeChange, changed_properties, style_invalidation_options);
}

bool Element::is_hidden() const
{
    if (layout_node() == nullptr)
        return true;
    if (layout_node()->computed_values().visibility() == CSS::Visibility::Hidden || layout_node()->computed_values().visibility() == CSS::Visibility::Collapse || layout_node()->computed_values().content_visibility() == CSS::ContentVisibility::Hidden)
        return true;
    for (ParentNode const* self_or_ancestor = this; self_or_ancestor; self_or_ancestor = self_or_ancestor->parent_or_shadow_host()) {
        if (self_or_ancestor->is_element() && static_cast<DOM::Element const*>(self_or_ancestor)->aria_hidden() == "true")
            return true;
    }
    return false;
}

bool Element::has_hidden_ancestor() const
{
    for (ParentNode const* self_or_ancestor = this; self_or_ancestor; self_or_ancestor = self_or_ancestor->parent_or_shadow_host()) {
        if (self_or_ancestor->is_element() && static_cast<DOM::Element const*>(self_or_ancestor)->is_hidden())
            return true;
    }
    return false;
}

bool Element::is_referenced() const
{
    bool is_referenced = false;
    if (id().has_value()) {
        root().for_each_in_subtree_of_type<HTML::HTMLElement>([&](auto& element) {
            auto aria_data = MUST(Web::ARIA::AriaData::build_data(element));
            if (aria_data->aria_labelled_by_or_default().contains_slow(id().value())) {
                is_referenced = true;
                return TraversalDecision::Break;
            }
            return TraversalDecision::Continue;
        });
    }
    return is_referenced;
}

bool Element::has_referenced_and_hidden_ancestor() const
{
    for (auto const* ancestor = parent_or_shadow_host(); ancestor; ancestor = ancestor->parent_or_shadow_host()) {
        if (ancestor->is_element())
            if (auto const* element = static_cast<DOM::Element const*>(ancestor); element->is_referenced() && element->is_hidden())
                return true;
    }
    return false;
}

// https://www.w3.org/TR/wai-aria-1.2/#tree_exclusion
bool Element::exclude_from_accessibility_tree() const
{
    // The following elements are not exposed via the accessibility API and user agents MUST NOT include them in the accessibility tree:

    // Elements, including their descendent elements, that have host language semantics specifying that the element is not displayed, such as CSS display:none, visibility:hidden, or the HTML hidden attribute.
    if (!layout_node())
        return true;

    // Elements with none or presentation as the first role in the role attribute. However, their exclusion is conditional. In addition, the element's descendants and text content are generally included. These exceptions and conditions are documented in the presentation (role) section.
    // FIXME: Handle exceptions to excluding presentation role
    auto role = role_or_default();
    if (role == ARIA::Role::none || role == ARIA::Role::presentation)
        return true;

    // TODO: If not already excluded from the accessibility tree per the above rules, user agents SHOULD NOT include the following elements in the accessibility tree:
    //    Elements, including their descendants, that have aria-hidden set to true. In other words, aria-hidden="true" on a parent overrides aria-hidden="false" on descendants.
    //    Any descendants of elements that have the characteristic "Children Presentational: True" unless the descendant is not allowed to be presentational because it meets one of the conditions for exception described in Presentational Roles Conflict Resolution. However, the text content of any excluded descendants is included.
    //    Elements with the following roles have the characteristic "Children Presentational: True":
    //      button
    //      checkbox
    //      img
    //      menuitemcheckbox
    //      menuitemradio
    //      meter
    //      option
    //      progressbar
    //      radio
    //      scrollbar
    //      separator
    //      slider
    //      switch
    //      tab
    return false;
}

// https://www.w3.org/TR/wai-aria-1.2/#tree_inclusion
bool Element::include_in_accessibility_tree() const
{
    // If not excluded from or marked as hidden in the accessibility tree per the rules above in Excluding Elements in the Accessibility Tree, user agents MUST provide an accessible object in the accessibility tree for DOM elements that meet any of the following criteria:
    if (exclude_from_accessibility_tree())
        return false;
    // Elements that are not hidden and may fire an accessibility API event, including:
    // Elements that are currently focused, even if the element or one of its ancestor elements has its aria-hidden attribute set to true.
    if (is_focused())
        return true;
    // TODO: Elements that are a valid target of an aria-activedescendant attribute.

    // Elements that have an explicit role or a global WAI-ARIA attribute and do not have aria-hidden set to true. (See Excluding Elements in the Accessibility Tree for additional guidance on aria-hidden.)
    // NOTE: The spec says only explicit roles count, but playing around in other browsers, this does not seem to be true in practice (for example button elements are always exposed with their implicit role if none is set)
    //       This issue https://github.com/w3c/aria/issues/1851 seeks clarification on this point
    if ((role_or_default().has_value() || has_global_aria_attribute()) && aria_hidden() != "true")
        return true;

    // TODO: Elements that are not hidden and have an ID that is referenced by another element via a WAI-ARIA property.

    return false;
}

// https://html.spec.whatwg.org/multipage/custom-elements.html#enqueue-an-element-on-the-appropriate-element-queue
void Element::enqueue_an_element_on_the_appropriate_element_queue()
{
    // 1. Let reactionsStack be element's relevant agent's custom element reactions stack.
    auto& relevant_agent = HTML::relevant_agent(*this);
    auto& reactions_stack = relevant_agent.custom_element_reactions_stack;

    // 2. If reactionsStack is empty, then:
    if (reactions_stack.element_queue_stack.is_empty()) {
        // 1. Add element to reactionsStack's backup element queue.
        reactions_stack.backup_element_queue.append(*this);

        // 2. If reactionsStack's processing the backup element queue flag is set, then return.
        if (reactions_stack.processing_the_backup_element_queue)
            return;

        // 3. Set reactionsStack's processing the backup element queue flag.
        reactions_stack.processing_the_backup_element_queue = true;

        // 4. Queue a microtask to perform the following steps:
        // NOTE: `this` is protected by GC::Function
        HTML::queue_a_microtask(&document(), GC::create_function(heap(), [this]() {
            auto& reactions_stack = HTML::relevant_agent(*this).custom_element_reactions_stack;

            // 1. Invoke custom element reactions in reactionsStack's backup element queue.
            Bindings::invoke_custom_element_reactions(reactions_stack.backup_element_queue);

            // 2. Unset reactionsStack's processing the backup element queue flag.
            reactions_stack.processing_the_backup_element_queue = false;
        }));

        return;
    }

    // 3. Otherwise, add element to element's relevant agent's current element queue.
    relevant_agent.current_element_queue().append(*this);
}

// https://html.spec.whatwg.org/multipage/custom-elements.html#enqueue-a-custom-element-upgrade-reaction
void Element::enqueue_a_custom_element_upgrade_reaction(HTML::CustomElementDefinition& custom_element_definition)
{
    // 1. Add a new upgrade reaction to element's custom element reaction queue, with custom element definition definition.
    ensure_custom_element_reaction_queue().append(CustomElementUpgradeReaction { .custom_element_definition = custom_element_definition });

    // 2. Enqueue an element on the appropriate element queue given element.
    enqueue_an_element_on_the_appropriate_element_queue();
}

void Element::enqueue_a_custom_element_callback_reaction(FlyString const& callback_name, GC::RootVector<JS::Value> arguments)
{
    // 1. Let definition be element's custom element definition.
    auto& definition = m_custom_element_definition;

    // 2. Let callback be the value of the entry in definition's lifecycle callbacks with key callbackName.
    auto callback_iterator = definition->lifecycle_callbacks().find(callback_name);

    // 3. If callback is null, then return.
    if (callback_iterator == definition->lifecycle_callbacks().end())
        return;

    if (!callback_iterator->value)
        return;

    // 4. If callbackName is "attributeChangedCallback", then:
    if (callback_name == HTML::CustomElementReactionNames::attributeChangedCallback) {
        // 1. Let attributeName be the first element of args.
        VERIFY(!arguments.is_empty());
        auto& attribute_name_value = arguments.first();
        VERIFY(attribute_name_value.is_string());
        auto attribute_name = attribute_name_value.as_string().utf8_string();

        // 2. If definition's observed attributes does not contain attributeName, then return.
        if (!definition->observed_attributes().contains_slow(attribute_name))
            return;
    }

    // 5. Add a new callback reaction to element's custom element reaction queue, with callback function callback and arguments args.
    ensure_custom_element_reaction_queue().append(CustomElementCallbackReaction { .callback = callback_iterator->value, .arguments = move(arguments) });

    // 6. Enqueue an element on the appropriate element queue given element.
    enqueue_an_element_on_the_appropriate_element_queue();
}

// https://html.spec.whatwg.org/multipage/custom-elements.html#concept-upgrade-an-element
JS::ThrowCompletionOr<void> Element::upgrade_element(GC::Ref<HTML::CustomElementDefinition> custom_element_definition)
{
    auto& realm = this->realm();
    auto& vm = this->vm();

    // 1. If element's custom element state is not "undefined" or "uncustomized", then return.
    if (m_custom_element_state != CustomElementState::Undefined && m_custom_element_state != CustomElementState::Uncustomized)
        return {};

    // 2. Set element's custom element definition to definition.
    m_custom_element_definition = custom_element_definition;

    // 3. Set element's custom element state to "failed".
    set_custom_element_state(CustomElementState::Failed);

    // 4. For each attribute in element's attribute list, in order, enqueue a custom element callback reaction with element, callback name "attributeChangedCallback",
    //    and « attribute's local name, null, attribute's value, attribute's namespace ».
    for (size_t attribute_index = 0; attribute_index < m_attributes->length(); ++attribute_index) {
        auto const* attribute = m_attributes->item(attribute_index);
        VERIFY(attribute);

        GC::RootVector<JS::Value> arguments { vm.heap() };

        arguments.append(JS::PrimitiveString::create(vm, attribute->local_name()));
        arguments.append(JS::js_null());
        arguments.append(JS::PrimitiveString::create(vm, attribute->value()));
        arguments.append(attribute->namespace_uri().has_value() ? JS::PrimitiveString::create(vm, attribute->namespace_uri().value()) : JS::js_null());

        enqueue_a_custom_element_callback_reaction(HTML::CustomElementReactionNames::attributeChangedCallback, move(arguments));
    }

    // 5. If element is connected, then enqueue a custom element callback reaction with element, callback name "connectedCallback", and « ».
    if (is_connected()) {
        GC::RootVector<JS::Value> empty_arguments { vm.heap() };
        enqueue_a_custom_element_callback_reaction(HTML::CustomElementReactionNames::connectedCallback, move(empty_arguments));
    }

    // 6. Add element to the end of definition's construction stack.
    custom_element_definition->construction_stack().append(GC::Ref { *this });

    // 7. Let C be definition's constructor.
    auto& constructor = custom_element_definition->constructor();

    // 8. Run the following substeps while catching any exceptions:
    auto attempt_to_construct_custom_element = [&]() -> JS::ThrowCompletionOr<void> {
        // 1. If definition's disable shadow is true and element's shadow root is non-null, then throw a "NotSupportedError" DOMException.
        if (custom_element_definition->disable_shadow() && shadow_root())
            return JS::throw_completion(WebIDL::NotSupportedError::create(realm, "Custom element definition disables shadow DOM and the custom element has a shadow root"_string));

        // 2. Set element's custom element state to "precustomized".
        set_custom_element_state(CustomElementState::Precustomized);

        // 3. Let constructResult be the result of constructing C, with no arguments.
        auto construct_result_optional = TRY(WebIDL::construct(constructor));
        VERIFY(construct_result_optional.has_value());
        auto construct_result = construct_result_optional.release_value();

        // 4. If SameValue(constructResult, element) is false, then throw a TypeError.
        if (!JS::same_value(construct_result, this))
            return vm.throw_completion<JS::TypeError>("Constructing the custom element returned a different element from the custom element"sv);

        return {};
    };

    auto maybe_exception = attempt_to_construct_custom_element();

    // Then, perform the following substep, regardless of whether the above steps threw an exception or not:
    // 1. Remove the last entry from the end of definition's construction stack.
    (void)custom_element_definition->construction_stack().take_last();

    // Finally, if the above steps threw an exception, then:
    if (maybe_exception.is_throw_completion()) {
        // 1. Set element's custom element definition to null.
        m_custom_element_definition = nullptr;

        // 2. Empty element's custom element reaction queue.
        if (m_custom_element_reaction_queue)
            m_custom_element_reaction_queue->clear();

        // 3. Rethrow the exception (thus terminating this algorithm).
        return maybe_exception.release_error();
    }

    // FIXME: 9. If element is a form-associated custom element, then:
    //           1. Reset the form owner of element. If element is associated with a form element, then enqueue a custom element callback reaction with element, callback name "formAssociatedCallback", and « the associated form ».
    //           2. If element is disabled, then enqueue a custom element callback reaction with element, callback name "formDisabledCallback" and « true ».

    // 10. Set element's custom element state to "custom".
    set_custom_element_state(CustomElementState::Custom);

    return {};
}

// https://html.spec.whatwg.org/multipage/custom-elements.html#concept-try-upgrade
void Element::try_to_upgrade()
{
    // 1. Let definition be the result of looking up a custom element definition given element's node document, element's namespace, element's local name, and element's is value.
    auto definition = document().lookup_custom_element_definition(namespace_uri(), local_name(), m_is_value);

    // 2. If definition is not null, then enqueue a custom element upgrade reaction given element and definition.
    if (definition)
        enqueue_a_custom_element_upgrade_reaction(*definition);
}

// https://dom.spec.whatwg.org/#concept-element-defined
bool Element::is_defined() const
{
    // An element whose custom element state is "uncustomized" or "custom" is said to be defined.
    return m_custom_element_state == CustomElementState::Uncustomized || m_custom_element_state == CustomElementState::Custom;
}

// https://dom.spec.whatwg.org/#concept-element-custom
bool Element::is_custom() const
{
    // An element whose custom element state is "custom" is said to be custom.
    return m_custom_element_state == CustomElementState::Custom;
}

void Element::set_custom_element_state(CustomElementState state)
{
    if (m_custom_element_state == state)
        return;
    m_custom_element_state = state;

    Vector<CSS::InvalidationSet::Property, 1> changed_properties;
    changed_properties.append({ .type = CSS::InvalidationSet::Property::Type::PseudoClass, .value = CSS::PseudoClass::Defined });
    invalidate_style(StyleInvalidationReason::CustomElementStateChange, changed_properties, {});
}

// https://html.spec.whatwg.org/multipage/dom.html#html-element-constructors
void Element::setup_custom_element_from_constructor(HTML::CustomElementDefinition& custom_element_definition, Optional<String> const& is_value)
{
    // 7.6. Set element's custom element state to "custom".
    set_custom_element_state(CustomElementState::Custom);

    // 7.7. Set element's custom element definition to definition.
    m_custom_element_definition = custom_element_definition;

    // 7.8. Set element's is value to is value.
    m_is_value = is_value;
}

void Element::set_prefix(Optional<FlyString> value)
{
    m_qualified_name.set_prefix(move(value));
}

// https://dom.spec.whatwg.org/#locate-a-namespace-prefix
Optional<String> Element::locate_a_namespace_prefix(Optional<String> const& namespace_) const
{
    // 1. If element’s namespace is namespace and its namespace prefix is non-null, then return its namespace prefix.
    if (this->namespace_uri() == namespace_ && this->prefix().has_value())
        return this->prefix()->to_string();

    // 2. If element has an attribute whose namespace prefix is "xmlns" and value is namespace, then return element’s first such attribute’s local name.
    if (auto* attributes = this->attributes()) {
        for (size_t i = 0; i < attributes->length(); ++i) {
            auto& attr = *attributes->item(i);
            if (attr.prefix() == "xmlns" && attr.value() == namespace_)
                return attr.local_name().to_string();
        }
    }

    // 3. If element’s parent element is not null, then return the result of running locate a namespace prefix on that element using namespace.
    if (auto* parent = this->parent_element())
        return parent->locate_a_namespace_prefix(namespace_);

    // 4. Return null
    return {};
}

void Element::for_each_attribute(Function<void(Attr const&)> callback) const
{
    for (size_t i = 0; i < m_attributes->length(); ++i)
        callback(*m_attributes->item(i));
}

void Element::for_each_attribute(Function<void(FlyString const&, String const&)> callback) const
{
    for_each_attribute([&callback](Attr const& attr) {
        callback(attr.name(), attr.value());
    });
}

GC::Ptr<Layout::NodeWithStyle> Element::layout_node()
{
    return static_cast<Layout::NodeWithStyle*>(Node::layout_node());
}

GC::Ptr<Layout::NodeWithStyle const> Element::layout_node() const
{
    return static_cast<Layout::NodeWithStyle const*>(Node::layout_node());
}

bool Element::has_attributes() const
{
    return !m_attributes->is_empty();
}

size_t Element::attribute_list_size() const
{
    return m_attributes->length();
}

GC::Ptr<CSS::CascadedProperties> Element::cascaded_properties(Optional<CSS::PseudoElement> pseudo_element) const
{
    if (pseudo_element.has_value()) {
        auto pseudo_element_data = get_pseudo_element(pseudo_element.value());
        if (pseudo_element_data.has_value())
            return pseudo_element_data->cascaded_properties;
        return nullptr;
    }
    return m_cascaded_properties;
}

void Element::set_cascaded_properties(Optional<CSS::PseudoElement> pseudo_element, GC::Ptr<CSS::CascadedProperties> cascaded_properties)
{
    if (pseudo_element.has_value()) {
        if (pseudo_element.value() >= CSS::PseudoElement::KnownPseudoElementCount)
            return;
        ensure_pseudo_element(pseudo_element.value()).cascaded_properties = cascaded_properties;
    } else {
        m_cascaded_properties = cascaded_properties;
    }
}

void Element::set_computed_properties(GC::Ptr<CSS::ComputedProperties> style)
{
    m_computed_properties = style;
    computed_properties_changed();
}

void Element::set_pseudo_element_computed_properties(CSS::PseudoElement pseudo_element, GC::Ptr<CSS::ComputedProperties> style)
{
    if (!m_pseudo_element_data && !style)
        return;

    if (!CSS::Selector::PseudoElementSelector::is_known_pseudo_element_type(pseudo_element)) {
        return;
    }

    ensure_pseudo_element(pseudo_element).computed_properties = style;
}

GC::Ptr<CSS::ComputedProperties> Element::pseudo_element_computed_properties(CSS::PseudoElement type)
{
    auto pseudo_element = get_pseudo_element(type);
    if (pseudo_element.has_value())
        return pseudo_element->computed_properties;
    return {};
}

Optional<Element::PseudoElement&> Element::get_pseudo_element(CSS::PseudoElement type) const
{
    if (!m_pseudo_element_data)
        return {};

    if (!CSS::Selector::PseudoElementSelector::is_known_pseudo_element_type(type)) {
        return {};
    }

    return m_pseudo_element_data->at(to_underlying(type));
}

Element::PseudoElement& Element::ensure_pseudo_element(CSS::PseudoElement type) const
{
    if (!m_pseudo_element_data)
        m_pseudo_element_data = make<PseudoElementData>();

    VERIFY(CSS::Selector::PseudoElementSelector::is_known_pseudo_element_type(type));

    return m_pseudo_element_data->at(to_underlying(type));
}

void Element::set_custom_properties(Optional<CSS::PseudoElement> pseudo_element, HashMap<FlyString, CSS::StyleProperty> custom_properties)
{
    if (!pseudo_element.has_value()) {
        m_custom_properties = move(custom_properties);
        return;
    }

    if (!CSS::Selector::PseudoElementSelector::is_known_pseudo_element_type(pseudo_element.value())) {
        return;
    }

    ensure_pseudo_element(pseudo_element.value()).custom_properties = move(custom_properties);
}

HashMap<FlyString, CSS::StyleProperty> const& Element::custom_properties(Optional<CSS::PseudoElement> pseudo_element) const
{
    if (!pseudo_element.has_value())
        return m_custom_properties;

    VERIFY(CSS::Selector::PseudoElementSelector::is_known_pseudo_element_type(pseudo_element.value()));

    return ensure_pseudo_element(pseudo_element.value()).custom_properties;
}

// https://drafts.csswg.org/cssom-view/#dom-element-scroll
void Element::scroll(double x, double y)
{
    // 1. If invoked with one argument, follow these substeps:
    //    NOTE: Not relevant here.
    // 2. If invoked with two arguments, follow these substeps:
    //     1. Let options be null converted to a ScrollToOptions dictionary. [WEBIDL]
    //     2. Let x and y be the arguments, respectively.
    //     3. Normalize non-finite values for x and y.
    //     4. Let the left dictionary member of options have the value x.
    //     5. Let the top dictionary member of options have the value y.
    x = HTML::normalize_non_finite_values(x);
    y = HTML::normalize_non_finite_values(y);

    // 3. Let document be the element’s node document.
    auto& document = this->document();

    // 4. If document is not the active document, terminate these steps.
    if (!document.is_active())
        return;

    // 5. Let window be the value of document’s defaultView attribute.
    // FIXME: The specification expects defaultView to be a Window object, but defaultView actually returns a WindowProxy object.
    auto window = document.window();

    // 6. If window is null, terminate these steps.
    if (!window)
        return;

    // 7. If the element is the root element and document is in quirks mode, terminate these steps.
    if (document.document_element() == this && document.in_quirks_mode())
        return;

    // NOTE: Ensure that layout is up-to-date before looking at metrics.
    document.update_layout(UpdateLayoutReason::ElementScroll);

    // 8. If the element is the root element invoke scroll() on window with scrollX on window as first argument and y as second argument, and terminate these steps.
    if (document.document_element() == this) {
        window->scroll(window->scroll_x(), y);
        return;
    }

    // 9. If the element is the body element, document is in quirks mode, and the element is not potentially scrollable, invoke scroll() on window
    //    with options as the only argument, and terminate these steps.
    if (document.body() == this && document.in_quirks_mode() && !is_potentially_scrollable()) {
        window->scroll(x, y);
        return;
    }

    // 10. If the element does not have any associated box, the element has no associated scrolling box, or the element has no overflow, terminate these steps.
    // FIXME: or the element has no overflow
    if (!paintable_box())
        return;

    // 11. Scroll the element to x,y, with the scroll behavior being the value of the behavior dictionary member of options.
    // FIXME: Implement this in terms of calling "scroll the element".
    auto scroll_offset = paintable_box()->scroll_offset();
    scroll_offset.set_x(CSSPixels::nearest_value_for(x));
    scroll_offset.set_y(CSSPixels::nearest_value_for(y));
    paintable_box()->set_scroll_offset(scroll_offset);
}

// https://drafts.csswg.org/cssom-view/#dom-element-scroll
void Element::scroll(HTML::ScrollToOptions options)
{
    // 1. If invoked with one argument, follow these substeps:
    //     1. Let options be the argument.
    //     2. Normalize non-finite values for left and top dictionary members of options, if present.
    //     3. Let x be the value of the left dictionary member of options, if present, or the element’s current scroll position on the x axis otherwise.
    //     4. Let y be the value of the top dictionary member of options, if present, or the element’s current scroll position on the y axis otherwise.
    // NOTE: remaining steps performed by Element::scroll(double x, double y)
    auto x = options.left.has_value() ? HTML::normalize_non_finite_values(options.left.value()) : scroll_left();
    auto y = options.top.has_value() ? HTML::normalize_non_finite_values(options.top.value()) : scroll_top();
    scroll(x, y);
}

// https://drafts.csswg.org/cssom-view/#dom-element-scrollby
void Element::scroll_by(double x, double y)
{
    // 1. Let options be null converted to a ScrollToOptions dictionary. [WEBIDL]
    HTML::ScrollToOptions options;

    // 2. Let x and y be the arguments, respectively.
    // 3. Normalize non-finite values for x and y.
    // 4. Let the left dictionary member of options have the value x.
    // 5. Let the top dictionary member of options have the value y.
    // NOTE: Element::scroll_by(HTML::ScrollToOptions) performs the normalization and following steps.
    options.left = x;
    options.top = y;
    scroll_by(options);
}

// https://drafts.csswg.org/cssom-view/#dom-element-scrollby
void Element::scroll_by(HTML::ScrollToOptions options)
{
    // 1. Let options be the argument.
    // 2. Normalize non-finite values for left and top dictionary members of options, if present.
    auto left = HTML::normalize_non_finite_values(options.left);
    auto top = HTML::normalize_non_finite_values(options.top);

    // 3. Add the value of scrollLeft to the left dictionary member.
    options.left = scroll_left() + left;

    // 4. Add the value of scrollTop to the top dictionary member.
    options.top = scroll_top() + top;

    // 5. Act as if the scroll() method was invoked with options as the only argument.
    scroll(options);
}

// https://drafts.csswg.org/cssom-view-1/#dom-element-checkvisibility
bool Element::check_visibility(Optional<CheckVisibilityOptions> options)
{
    // NOTE: Ensure that layout is up-to-date before looking at metrics.
    document().update_layout(UpdateLayoutReason::ElementCheckVisibility);

    // 1. If this does not have an associated box, return false.
    if (!paintable_box())
        return false;

    // 2. If an ancestor of this in the flat tree has content-visibility: hidden, return false.
    for (auto* element = parent_element(); element; element = element->parent_element()) {
        if (element->computed_properties()->content_visibility() == CSS::ContentVisibility::Hidden)
            return false;
    }

    // AD-HOC: Since the rest of the steps use the options, we can return early if we haven't been given any options.
    if (!options.has_value())
        return true;

    // 3. If either the opacityProperty or the checkOpacity dictionary members of options are true, and this, or an ancestor of this in the flat tree, has a computed opacity value of 0, return false.
    if (options->opacity_property || options->check_opacity) {
        for (auto* element = this; element; element = element->parent_element()) {
            if (element->computed_properties()->opacity() == 0.0f)
                return false;
        }
    }

    // 4. If either the visibilityProperty or the checkVisibilityCSS dictionary members of options are true, and this is invisible, return false.
    if (options->visibility_property || options->check_visibility_css) {
        if (computed_properties()->visibility() == CSS::Visibility::Hidden)
            return false;
    }

    // 5. If the contentVisibilityAuto dictionary member of options is true and an ancestor of this in the flat tree skips its contents due to content-visibility: auto, return false.
    // FIXME: Currently we do not skip any content if content-visibility is auto: https://drafts.csswg.org/css-contain-2/#proximity-to-the-viewport
    auto const skipped_contents_due_to_content_visibility_auto = false;
    if (options->content_visibility_auto && skipped_contents_due_to_content_visibility_auto) {
        for (auto* element = this; element; element = element->parent_element()) {
            if (element->computed_properties()->content_visibility() == CSS::ContentVisibility::Auto)
                return false;
        }
    }

    // 6. Return true.
    return true;
}

// https://drafts.csswg.org/css-contain/#proximity-to-the-viewport
void Element::determine_proximity_to_the_viewport()
{
    // An element that has content-visibility: auto is in one of three states when it comes to its proximity to the viewport:

    // - The element is close to the viewport: In this state, the element is considered "on-screen": its paint
    //   containment box's overflow clip edge intersects with the viewport, or a user-agent defined margin around the
    //   viewport.
    auto viewport_rect = document().viewport_rect();
    // NOTE: This margin is meant to allow the user agent to begin preparing for an element to be in the
    // viewport soon. A margin of 50% is suggested as a reasonable default.
    viewport_rect.inflate(viewport_rect.width(), viewport_rect.height());
    // FIXME: We don't have paint containment or the overflow clip edge yet, so this is just using the absolute rect for now.
    if (paintable_box()->absolute_rect().intersects(viewport_rect))
        m_proximity_to_the_viewport = ProximityToTheViewport::CloseToTheViewport;

    // FIXME: If a filter (see [FILTER-EFFECTS-1]) with non local effects includes the element as part of its input, the user
    //        agent should also treat the element as relevant to the user when the filter’s output can affect the rendering
    //        within the viewport (or within the user-agent defined margin around the viewport), even if the element itself is
    //        still off-screen.

    // - The element is far away from the viewport: In this state, the element’s proximity to the viewport has been
    //   computed and is not close to the viewport.
    m_proximity_to_the_viewport = ProximityToTheViewport::FarAwayFromTheViewport;

    // - The element’s proximity to the viewport is not determined: In this state, the computation to determine the
    //   element’s proximity to the viewport has not been done since the last time the element was connected.
    // NOTE: This function is what does the computation to determine the element’s proximity to the viewport, so this is not the case.
}

// https://drafts.csswg.org/css-contain/#relevant-to-the-user
bool Element::is_relevant_to_the_user()
{
    // An element is relevant to the user if any of the following conditions are true:

    // The element is close to the viewport.
    if (m_proximity_to_the_viewport == ProximityToTheViewport::CloseToTheViewport)
        return true;

    // Either the element or its contents are focused, as described in the focus section of the HTML spec.
    auto* focused_element = document().focused_element();
    if (focused_element && is_inclusive_ancestor_of(*focused_element))
        return true;

    // Either the element or its contents are selected, where selection is described in the selection API.
    if (document().get_selection()->contains_node(*this, true))
        return true;

    // Either the element or its contents are placed in the top layer.
    bool is_in_top_layer = false;
    for_each_in_inclusive_subtree_of_type<Element>([&](auto& element) {
        if (element.in_top_layer()) {
            is_in_top_layer = true;
            return TraversalDecision::Break;
        }

        return TraversalDecision::Continue;
    });
    if (is_in_top_layer)
        return true;

    // FIXME: The element has a flat tree descendant that is captured in a view transition.

    // NOTE: none of the above conditions are true, so the element is not relevant to the user.
    return false;
}

// https://drafts.csswg.org/css-contain-2/#skips-its-contents
bool Element::skips_its_contents()
{
    // https://drafts.csswg.org/css-contain-2/#valdef-content-visibility-hidden
    // The element skips its contents.
    if (computed_properties()->content_visibility() == CSS::ContentVisibility::Hidden)
        return true;

    // https://drafts.csswg.org/css-contain-2/#valdef-content-visibility-auto
    // If the element is not relevant to the user, it also skips its contents.
    if (computed_properties()->content_visibility() == CSS::ContentVisibility::Auto && !this->is_relevant_to_the_user()) {
        return true;
    }

    return false;
}

// https://drafts.csswg.org/css-contain-2/#containment-size
bool Element::has_size_containment() const
{
    // However, giving an element size containment has no effect if any of the following are true:

    // - if the element does not generate a principal box (as is the case with 'display: contents' or 'display: none')
    if (!layout_node())
        return false;

    // - if its inner display type is 'table'
    if (layout_node()->display().is_table_inside())
        return false;

    // - if its principal box is an internal table box
    if (layout_node()->display().is_internal_table())
        return false;

    // - if its principal box is an internal ruby box or a non-atomic inline-level box
    // FIXME: Implement this.

    if (computed_properties()->contain().size_containment)
        return true;

    return false;
}
// https://drafts.csswg.org/css-contain-2/#containment-inline-size
bool Element::has_inline_size_containment() const
{
    // Giving an element inline-size containment has no effect if any of the following are true:

    // - if the element does not generate a principal box (as is the case with 'display: contents' or 'display: none')
    if (!layout_node())
        return false;

    // - if its inner display type is 'table'
    if (layout_node()->display().is_table_inside())
        return false;

    // - if its principal box is an internal table box
    if (layout_node()->display().is_internal_table())
        return false;

    // - if its principal box is an internal ruby box or a non-atomic inline-level box
    // FIXME: Implement this.

    if (computed_properties()->contain().inline_size_containment)
        return true;

    return false;
}
// https://drafts.csswg.org/css-contain-2/#containment-layout
bool Element::has_layout_containment() const
{
    // However, giving an element layout containment has no effect if any of the following are true:

    // - if the element does not generate a principal box (as is the case with 'display: contents' or 'display: none')
    if (!layout_node())
        return false;

    // - if its principal box is an internal table box other than 'table-cell'
    if (layout_node()->display().is_internal_table() && !layout_node()->display().is_table_cell())
        return false;

    // - if its principal box is an internal ruby box or a non-atomic inline-level box
    // FIXME: Implement this.

    if (computed_properties()->contain().layout_containment)
        return true;

    // https://drafts.csswg.org/css-contain-2/#valdef-content-visibility-auto
    // Changes the used value of the 'contain' property so as to turn on layout containment, style containment, and
    // paint containment for the element.
    if (computed_properties()->content_visibility() == CSS::ContentVisibility::Auto)
        return true;

    return false;
}
// https://drafts.csswg.org/css-contain-2/#containment-style
bool Element::has_style_containment() const
{
    // However, giving an element style containment has no effect if any of the following are true:

    // - if the element does not generate a principal box (as is the case with 'display: contents' or 'display: none')
    if (!layout_node())
        return false;

    if (computed_properties()->contain().style_containment)
        return true;

    // https://drafts.csswg.org/css-contain-2/#valdef-content-visibility-auto
    // Changes the used value of the 'contain' property so as to turn on layout containment, style containment, and
    // paint containment for the element.
    if (computed_properties()->content_visibility() == CSS::ContentVisibility::Auto)
        return true;

    return false;
}
// https://drafts.csswg.org/css-contain-2/#containment-paint
bool Element::has_paint_containment() const
{
    // However, giving an element paint containment has no effect if any of the following are true:

    // - if the element does not generate a principal box (as is the case with 'display: contents' or 'display: none')
    if (!layout_node())
        return false;

    // - if its principal box is an internal table box other than 'table-cell'
    if (layout_node()->display().is_internal_table() && !layout_node()->display().is_table_cell())
        return false;

    // - if its principal box is an internal ruby box or a non-atomic inline-level box
    // FIXME: Implement this

    if (computed_properties()->contain().paint_containment)
        return true;

    // https://drafts.csswg.org/css-contain-2/#valdef-content-visibility-auto
    // Changes the used value of the 'contain' property so as to turn on layout containment, style containment, and
    // paint containment for the element.
    if (computed_properties()->content_visibility() == CSS::ContentVisibility::Auto)
        return true;

    return false;
}

size_t Element::number_of_owned_list_items() const
{
    auto number_of_owned_li_elements = 0;
    for_each_child_of_type<DOM::Element>([&](auto& child) {
        if (child.list_owner() == this) {
            number_of_owned_li_elements++;
        }
        return IterationDecision::Continue;
    });
    return number_of_owned_li_elements;
}

// https://html.spec.whatwg.org/multipage/grouping-content.html#list-owner
Element const* Element::list_owner() const
{
    // Any element whose computed value of 'display' is 'list-item' has a list owner, which is determined as follows:
    if (!computed_properties()->display().is_list_item())
        return nullptr;

    // 1. If the element is not being rendered, return null; the element has no list owner.
    if (!layout_node())
        return nullptr;

    // 2. Let ancestor be the element's parent.
    auto const* ancestor = parent_element();

    // 3. If the element has an ol, ul, or menu ancestor, set ancestor to the closest such ancestor element.
    for_each_ancestor([&ancestor](GC::Ref<Node> node) {
        if (is<HTML::HTMLOListElement>(*node) || is<HTML::HTMLUListElement>(*node) || is<HTML::HTMLMenuElement>(*node)) {
            ancestor = static_cast<Element const*>(node.ptr());
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });

    // 4. Return the closest inclusive ancestor of ancestor that produces a CSS box.
    // Spec-Note: Such an element will always exist, as at the very least the document element will always produce a CSS box.
    ancestor->for_each_inclusive_ancestor([&ancestor](GC::Ref<Node> node) {
        if (is<Element>(*node) && node->paintable_box()) {
            ancestor = static_cast<Element const*>(node.ptr());
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    return ancestor;
}

// https://html.spec.whatwg.org/multipage/grouping-content.html#ordinal-value
size_t Element::ordinal_value() const
{
    // NOTE: The spec provides an algorithm to determine the ordinal value of each element owned by a given list owner.
    //       However, we are only interested in the ordinal value of this element.

    // FIXME: 1. Let i be 1.

    // 2. If owner is an ol element, let numbering be owner's starting value. Otherwise, let numbering be 1.
    auto const* owner = list_owner();
    if (!owner) {
        return 1;
    }

    auto numbering = 1;
    auto reversed = false;
    if (is<HTML::HTMLOListElement>(owner)) {
        auto const* ol_element = static_cast<const HTML::HTMLOListElement*>(owner);
        numbering = ol_element->starting_value();
        reversed = ol_element->has_attribute(HTML::AttributeNames::reversed);
    }

    // FIXME: 3. Loop : If i is greater than the number of list items that owner owns, then return; all of owner's owned list items have been assigned ordinal values.
    // FIXME: 4. Let item be the ith of owner's owned list items, in tree order.

    owner->for_each_child_of_type<DOM::Element>([&](auto& item) {
        if (item.list_owner() == owner) {
            // 5. If item is an li element that has a value attribute, then:
            auto value_attribute = item.get_attribute(HTML::AttributeNames::value);
            if (is<HTML::HTMLLIElement>(item) && value_attribute.has_value()) {
                // 1. Let parsed be the result of parsing the value of the attribute as an integer.
                auto parsed = HTML::parse_integer(value_attribute.value());

                // 2. If parsed is not an error, then set numbering to parsed.
                if (parsed.has_value())
                    numbering = parsed.value();
            }

            // FIXME: 6. The ordinal value of item is numbering.
            if (&item == this)
                return IterationDecision::Break;

            // 7. If owner is an ol element, and owner has a reversed attribute, decrement numbering by 1; otherwise, increment numbering by 1.
            if (reversed) {
                numbering--;
            } else {
                numbering++;
            }

            // FIXME: 8. Increment i by 1.
        }
        return IterationDecision::Continue;
    });

    // FIXME: 9. Go to the step labeled loop.
    return numbering;
}

bool Element::id_reference_exists(String const& id_reference) const
{
    return document().get_element_by_id(id_reference);
}

void Element::register_intersection_observer(Badge<IntersectionObserver::IntersectionObserver>, IntersectionObserver::IntersectionObserverRegistration registration)
{
    if (!m_registered_intersection_observers)
        m_registered_intersection_observers = make<Vector<IntersectionObserver::IntersectionObserverRegistration>>();
    m_registered_intersection_observers->append(move(registration));
}

void Element::unregister_intersection_observer(Badge<IntersectionObserver::IntersectionObserver>, GC::Ref<IntersectionObserver::IntersectionObserver> observer)
{
    if (!m_registered_intersection_observers)
        return;
    m_registered_intersection_observers->remove_first_matching([&observer](IntersectionObserver::IntersectionObserverRegistration const& entry) {
        return entry.observer == observer;
    });
}

IntersectionObserver::IntersectionObserverRegistration& Element::get_intersection_observer_registration(Badge<DOM::Document>, IntersectionObserver::IntersectionObserver const& observer)
{
    VERIFY(m_registered_intersection_observers);
    auto registration_iterator = m_registered_intersection_observers->find_if([&observer](IntersectionObserver::IntersectionObserverRegistration const& entry) {
        return entry.observer.ptr() == &observer;
    });
    VERIFY(!registration_iterator.is_end());
    return *registration_iterator;
}

// https://html.spec.whatwg.org/multipage/dom.html#the-directionality
Element::Directionality Element::directionality() const
{
    // The directionality of an element (any element, not just an HTML element) is either 'ltr' or 'rtl'.
    // To compute the directionality given an element element, switch on element's dir attribute state:
    auto maybe_dir = this->dir();
    if (maybe_dir.has_value()) {
        auto dir = maybe_dir.release_value();
        switch (dir) {
        // -> ltr
        case Dir::Ltr:
            // Return 'ltr'.
            return Directionality::Ltr;
        // -> rtl
        case Dir::Rtl:
            // Return 'rtl'.
            return Directionality::Rtl;
        // -> auto
        case Dir::Auto:
            // 1. Let result be the auto directionality of element.
            auto result = auto_directionality();

            // 2. If result is null, then return 'ltr'.
            if (!result.has_value())
                return Directionality::Ltr;

            // 3. Return result.
            return result.release_value();
        }
    }
    // -> undefined
    VERIFY(!maybe_dir.has_value());

    // If element is a bdi element:
    if (local_name() == HTML::TagNames::bdi) {
        // 1. Let result be the auto directionality of element.
        auto result = auto_directionality();

        // 2. If result is null, then return 'ltr'.
        if (!result.has_value())
            return Directionality::Ltr;

        // 3. Return result.
        return result.release_value();
    }

    // If element is an input element whose type attribute is in the Telephone state:
    if (is<HTML::HTMLInputElement>(this) && static_cast<HTML::HTMLInputElement const&>(*this).type_state() == HTML::HTMLInputElement::TypeAttributeState::Telephone) {
        // Return 'ltr'.
        return Directionality::Ltr;
    }

    // Otherwise:
    // Return the parent directionality of element.
    return parent_directionality();
}

// https://html.spec.whatwg.org/multipage/dom.html#auto-directionality-form-associated-elements
bool Element::is_auto_directionality_form_associated_element() const
{
    // The auto-directionality form-associated elements are:
    // input elements whose type attribute is in the Hidden, Text, Search, Telephone, URL, Email, Password, Submit Button, Reset Button, or Button state,
    // and textarea elements.
    return is<HTML::HTMLTextAreaElement>(this)
        || (is<HTML::HTMLInputElement>(this) && first_is_one_of(static_cast<HTML::HTMLInputElement const&>(*this).type_state(), HTML::HTMLInputElement::TypeAttributeState::Hidden, HTML::HTMLInputElement::TypeAttributeState::Text, HTML::HTMLInputElement::TypeAttributeState::Search, HTML::HTMLInputElement::TypeAttributeState::Telephone, HTML::HTMLInputElement::TypeAttributeState::URL, HTML::HTMLInputElement::TypeAttributeState::Email, HTML::HTMLInputElement::TypeAttributeState::Password, HTML::HTMLInputElement::TypeAttributeState::SubmitButton, HTML::HTMLInputElement::TypeAttributeState::ResetButton, HTML::HTMLInputElement::TypeAttributeState::Button));
}

// https://html.spec.whatwg.org/multipage/dom.html#auto-directionality
Optional<Element::Directionality> Element::auto_directionality() const
{
    // 1. If element is an auto-directionality form-associated element:
    if (is_auto_directionality_form_associated_element()) {
        auto const* form_associated_element = dynamic_cast<HTML::FormAssociatedElement const*>(this);
        VERIFY(form_associated_element);
        auto const& value = form_associated_element->value();

        // 1. If element's value contains a character of bidirectional character type AL or R,
        //    and there is no character of bidirectional character type L anywhere before it in the element's value, then return 'rtl'.
        for (auto code_point : Utf8View(value)) {
            auto bidi_class = Unicode::bidirectional_class(code_point);
            if (bidi_class == Unicode::BidiClass::LeftToRight)
                break;
            if (bidi_class == Unicode::BidiClass::RightToLeftArabic || bidi_class == Unicode::BidiClass::RightToLeft)
                return Directionality::Rtl;
        }

        // 2. If element's value is not the empty string, then return 'ltr'.
        if (value.is_empty())
            return Directionality::Ltr;

        // 3. Return null.
        return {};
    }

    // 2. If element is a slot element whose root is a shadow root and element's assigned nodes are not empty:
    if (is<HTML::HTMLSlotElement>(this)) {
        auto const& slot = static_cast<HTML::HTMLSlotElement const&>(*this);
        if (slot.root().is_shadow_root() && !slot.assigned_nodes().is_empty()) {
            // 1 . For each node child of element's assigned nodes:
            for (auto const& child : slot.assigned_nodes()) {
                // 1. Let childDirection be null.
                Optional<Directionality> child_direction;

                // 2. If child is a Text node, then set childDirection to the text node directionality of child.
                if (child->is_text())
                    child_direction = static_cast<Text const&>(*child).directionality();

                // 3. Otherwise:
                else {
                    // 1. Assert: child is an Element node.
                    VERIFY(child->is_element());

                    // 2. Set childDirection to the contained text auto directionality of child with canExcludeRoot set to true.
                    child_direction = static_cast<Element const&>(*child).contained_text_auto_directionality(true);
                }

                // 4. If childDirection is not null, then return childDirection.
                if (child_direction.has_value())
                    return child_direction;
            }

            // 2. Return null.
            return {};
        }
    }

    // 3. Return the contained text auto directionality of element with canExcludeRoot set to false.
    return contained_text_auto_directionality(false);
}

// https://html.spec.whatwg.org/multipage/dom.html#contained-text-auto-directionality
Optional<Element::Directionality> Element::contained_text_auto_directionality(bool can_exclude_root) const
{
    // To compute the contained text auto directionality of an element element with a boolean canExcludeRoot:

    // 1. For each node descendant of element's descendants, in tree order:
    Optional<Directionality> result;
    for_each_in_subtree([&](auto& descendant) {
        // 1. If any of
        //    - descendant
        //    - any ancestor element of descendant that is a descendant of element
        //    - if canExcludeRoot is true, element
        //    is one of
        //    - FIXME: a bdi element
        //    - a script element
        //    - a style element
        //    - a textarea element
        //    - an element whose dir attribute is not in the undefined state
        //    then continue.
        // NOTE: "any ancestor element of descendant that is a descendant of element" will be iterated already.
        auto is_one_of_the_filtered_elements = [](auto& descendant) -> bool {
            return is<HTML::HTMLScriptElement>(descendant)
                || is<HTML::HTMLStyleElement>(descendant)
                || is<HTML::HTMLTextAreaElement>(descendant)
                || (is<Element>(descendant) && static_cast<Element const&>(descendant).dir().has_value());
        };
        if (is_one_of_the_filtered_elements(descendant)
            || (can_exclude_root && is_one_of_the_filtered_elements(*this))) {
            return TraversalDecision::SkipChildrenAndContinue;
        }

        // 2. If descendant is a slot element whose root is a shadow root, then return the directionality of that shadow root's host.
        if (is<HTML::HTMLSlotElement>(descendant)) {
            auto const& root = static_cast<HTML::HTMLSlotElement const&>(descendant).root();
            if (root.is_shadow_root()) {
                auto const& host = static_cast<ShadowRoot const&>(root).host();
                VERIFY(host);
                result = host->directionality();
                return TraversalDecision::Break;
            }
        }

        // 3. If descendant is not a Text node, then continue.
        if (!descendant.is_text())
            return TraversalDecision::Continue;

        // 4. Let result be the text node directionality of descendant.
        result = static_cast<Text const&>(descendant).directionality();

        // 5. If result is not null, then return result.
        if (result.has_value())
            return TraversalDecision::Break;

        return TraversalDecision::Continue;
    });

    if (result.has_value())
        return result;

    // 2. Return null.
    return {};
}

// https://html.spec.whatwg.org/multipage/dom.html#parent-directionality
Element::Directionality Element::parent_directionality() const
{
    // 1. Let parentNode be element's parent node.
    auto const* parent_node = this->parent_node();

    // 2. If parentNode is a shadow root, then return the directionality of parentNode's host.
    if (is<ShadowRoot>(parent_node)) {
        auto const& host = static_cast<ShadowRoot const&>(*parent_node).host();
        VERIFY(host);
        return host->directionality();
    }

    // 3. If parentNode is an element, then return the directionality of parentNode.
    if (is<Element>(parent_node))
        return static_cast<Element const&>(*parent_node).directionality();

    // 4. Return 'ltr'.
    return Directionality::Ltr;
}

// https://dom.spec.whatwg.org/#concept-element-attributes-change-ext
void Element::attribute_changed(FlyString const& local_name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    // https://dom.spec.whatwg.org/#ref-for-concept-element-attributes-change-ext①
    // 1. If localName is slot and namespace is null, then:
    if (local_name == HTML::AttributeNames::slot && !namespace_.has_value()) {
        // 1. If value is oldValue, then return.
        if (value == old_value)
            return;

        // 2. If value is null and oldValue is the empty string, then return.
        if (!value.has_value() && old_value == String {})
            return;

        // 3. If value is the empty string and oldValue is null, then return.
        if (value == String {} && !old_value.has_value())
            return;

        // 4. If value is null or the empty string, then set element’s name to the empty string.
        if (!value.has_value() || value->is_empty())
            set_slottable_name({});
        // 5. Otherwise, set element’s name to value.
        else
            set_slottable_name(*value);

        // 6. If element is assigned, then run assign slottables for element’s assigned slot.
        if (auto assigned_slot = assigned_slot_internal())
            assign_slottables(*assigned_slot);

        // 7. Run assign a slot for element.
        assign_a_slot(GC::Ref { *this });
        return;
    }

    auto value_or_empty = value.value_or(String {});

    if (local_name == HTML::AttributeNames::id) {
        if (value_or_empty.is_empty())
            m_id = {};
        else
            m_id = value_or_empty;

        if (is_connected()) {
            Optional<FlyString> old_value_fly_string;
            if (old_value.has_value())
                old_value_fly_string = *old_value;
            document().element_id_changed({}, *this, old_value_fly_string);
        }
    } else if (local_name == HTML::AttributeNames::name) {
        if (value_or_empty.is_empty())
            m_name = {};
        else
            m_name = value_or_empty;

        if (is_connected())
            document().element_name_changed({}, *this);
    } else if (local_name == HTML::AttributeNames::class_) {
        if (value_or_empty.is_empty()) {
            m_classes.clear();
        } else {
            auto new_classes = value_or_empty.bytes_as_string_view().split_view_if(Infra::is_ascii_whitespace);
            m_classes.clear();
            m_classes.ensure_capacity(new_classes.size());
            for (auto& new_class : new_classes) {
                m_classes.unchecked_append(FlyString::from_utf8(new_class).release_value_but_fixme_should_propagate_errors());
            }
        }
        if (m_class_list)
            m_class_list->associated_attribute_changed(value_or_empty);
    } else if (local_name == HTML::AttributeNames::style) {
        if (!value.has_value()) {
            if (m_inline_style) {
                m_inline_style = nullptr;
                set_needs_style_update(true);
            }
        } else {
            // https://drafts.csswg.org/cssom/#ref-for-cssstyledeclaration-updating-flag
            if (m_inline_style && m_inline_style->is_updating())
                return;
            if (!m_inline_style)
                m_inline_style = CSS::CSSStyleProperties::create_element_inline_style({ *this }, {}, {});
            m_inline_style->set_declarations_from_text(*value);
            set_needs_style_update(true);
        }
    } else if (local_name == HTML::AttributeNames::dir) {
        // https://html.spec.whatwg.org/multipage/dom.html#attr-dir
        if (value_or_empty.equals_ignoring_ascii_case("ltr"sv))
            m_dir = Dir::Ltr;
        else if (value_or_empty.equals_ignoring_ascii_case("rtl"sv))
            m_dir = Dir::Rtl;
        else if (value_or_empty.equals_ignoring_ascii_case("auto"sv))
            m_dir = Dir::Auto;
        else
            m_dir = {};
    } else if (local_name == ARIA::AttributeNames::aria_active_descendant) {
        // https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#reflecting-content-attributes-in-idl-attributes:concept-element-attributes-change-ext
        // Set element's explicitly set attr-element to null.
        m_aria_active_descendant_element = nullptr;
    }
}

auto Element::ensure_custom_element_reaction_queue() -> CustomElementReactionQueue&
{
    if (!m_custom_element_reaction_queue)
        m_custom_element_reaction_queue = make<CustomElementReactionQueue>();
    return *m_custom_element_reaction_queue;
}

CSS::StyleSheetList& Element::document_or_shadow_root_style_sheets()
{
    auto& root_node = root();
    if (is<DOM::ShadowRoot>(root_node))
        return static_cast<DOM::ShadowRoot&>(root_node).style_sheets();

    return document().style_sheets();
}

ElementByIdMap& Element::document_or_shadow_root_element_by_id_map()
{
    auto& root_node = root();
    if (is<ShadowRoot>(root_node))
        return static_cast<ShadowRoot&>(root_node).element_by_id();
    return document().element_by_id();
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-element-gethtml
WebIDL::ExceptionOr<String> Element::get_html(GetHTMLOptions const& options) const
{
    // Element's getHTML(options) method steps are to return the result
    // of HTML fragment serialization algorithm with this,
    // options["serializableShadowRoots"], and options["shadowRoots"].
    return HTML::HTMLParser::serialize_html_fragment(
        *this,
        options.serializable_shadow_roots ? HTML::HTMLParser::SerializableShadowRoots::Yes : HTML::HTMLParser::SerializableShadowRoots::No,
        options.shadow_roots);
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-element-sethtmlunsafe
WebIDL::ExceptionOr<void> Element::set_html_unsafe(StringView html)
{
    // FIXME: 1. Let compliantHTML be the result of invoking the Get Trusted Type compliant string algorithm with TrustedHTML, this's relevant global object, html, "Element setHTMLUnsafe", and "script".

    // 2. Let target be this's template contents if this is a template element; otherwise this.
    DOM::Node* target = this;
    if (is<HTML::HTMLTemplateElement>(*this))
        target = as<HTML::HTMLTemplateElement>(*this).content().ptr();

    // 3. Unsafe set HTML given target, this, and compliantHTML. FIXME: Use compliantHTML.
    TRY(target->unsafely_set_html(*this, html));

    return {};
}

Optional<CSS::CountersSet const&> Element::counters_set()
{
    if (!m_counters_set)
        return {};
    return *m_counters_set;
}

CSS::CountersSet& Element::ensure_counters_set()
{
    if (!m_counters_set)
        m_counters_set = make<CSS::CountersSet>();
    return *m_counters_set;
}

// https://drafts.csswg.org/css-lists-3/#auto-numbering
void Element::resolve_counters(CSS::ComputedProperties& style)
{
    // Resolving counter values on a given element is a multi-step process:

    // 1. Existing counters are inherited from previous elements.
    inherit_counters();

    // https://drafts.csswg.org/css-lists-3/#counters-without-boxes
    // An element that does not generate a box (for example, an element with display set to none,
    // or a pseudo-element with content set to none) cannot set, reset, or increment a counter.
    // The counter properties are still valid on such an element, but they must have no effect.
    if (style.display().is_none())
        return;

    // 2. New counters are instantiated (counter-reset).
    auto counter_reset = style.counter_data(CSS::PropertyID::CounterReset);
    for (auto const& counter : counter_reset)
        ensure_counters_set().instantiate_a_counter(counter.name, unique_id(), counter.is_reversed, counter.value);

    // FIXME: Take style containment into account
    // https://drafts.csswg.org/css-contain-2/#containment-style
    // Giving an element style containment has the following effects:
    // 1. The 'counter-increment' and 'counter-set' properties must be scoped to the element’s sub-tree and create a
    //    new counter.

    // 3. Counter values are incremented (counter-increment).
    auto counter_increment = style.counter_data(CSS::PropertyID::CounterIncrement);
    for (auto const& counter : counter_increment)
        ensure_counters_set().increment_a_counter(counter.name, unique_id(), *counter.value);

    // 4. Counter values are explicitly set (counter-set).
    auto counter_set = style.counter_data(CSS::PropertyID::CounterSet);
    for (auto const& counter : counter_set)
        ensure_counters_set().set_a_counter(counter.name, unique_id(), *counter.value);

    // 5. Counter values are used (counter()/counters()).
    // NOTE: This happens when we process the `content` property.
}

// https://drafts.csswg.org/css-lists-3/#inherit-counters
void Element::inherit_counters()
{
    // 1. If element is the root of its document tree, the element has an initially-empty CSS counters set.
    //    Return.
    auto* parent = parent_element();
    if (parent == nullptr) {
        // NOTE: We represent an empty counters set with `m_counters_set = nullptr`.
        m_counters_set = nullptr;
        return;
    }

    // 2. Let element counters, representing element’s own CSS counters set, be a copy of the CSS counters
    //    set of element’s parent element.
    OwnPtr<CSS::CountersSet> element_counters;
    // OPTIMIZATION: If parent has a set, we create a copy. Otherwise, we avoid allocating one until we need
    // to add something to it.
    auto ensure_element_counters = [&]() {
        if (!element_counters)
            element_counters = make<CSS::CountersSet>();
    };
    if (parent->has_non_empty_counters_set()) {
        element_counters = make<CSS::CountersSet>();
        *element_counters = *parent_element()->counters_set();
    }

    // 3. Let sibling counters be the CSS counters set of element’s preceding sibling (if it has one),
    //    or an empty CSS counters set otherwise.
    //    For each counter of sibling counters, if element counters does not already contain a counter with
    //    the same name, append a copy of counter to element counters.
    if (auto* const sibling = previous_sibling_of_type<Element>(); sibling && sibling->has_non_empty_counters_set()) {
        auto& sibling_counters = sibling->counters_set().release_value();
        ensure_element_counters();
        for (auto const& counter : sibling_counters.counters()) {
            if (!element_counters->last_counter_with_name(counter.name).has_value())
                element_counters->append_copy(counter);
        }
    }

    // 4. Let value source be the CSS counters set of the element immediately preceding element in tree order.
    //    For each source counter of value source, if element counters contains a counter with the same name
    //    and creator, then set the value of that counter to source counter’s value.
    if (auto* const previous = previous_element_in_pre_order(); previous && previous->has_non_empty_counters_set()) {
        // NOTE: If element_counters is empty (AKA null) then we can skip this since nothing will match.
        if (element_counters) {
            auto& value_source = previous->counters_set().release_value();
            for (auto const& source_counter : value_source.counters()) {
                auto maybe_existing_counter = element_counters->counter_with_same_name_and_creator(source_counter.name, source_counter.originating_element_id);
                if (maybe_existing_counter.has_value())
                    maybe_existing_counter->value = source_counter.value;
            }
        }
    }

    VERIFY(!element_counters || !element_counters->is_empty());
    m_counters_set = move(element_counters);
}

// https://html.spec.whatwg.org/multipage/dom.html#the-lang-and-xml:lang-attributes
Optional<String> Element::lang() const
{
    auto attempt_to_determine_lang_attribute = [&]() -> Optional<String> {
        // 1. If the node is an element that has a lang attribute in the XML namespace set
        //      Use the value of that attribute.
        auto maybe_xml_lang = get_attribute_ns(Namespace::XML, HTML::AttributeNames::lang);
        if (maybe_xml_lang.has_value())
            return maybe_xml_lang.release_value();

        // 2. If the node is an HTML element or an element in the SVG namespace, and it has a lang in no namespace attribute set
        //      Use the value of that attribute.
        if (is_html_element() || namespace_uri() == Namespace::SVG) {
            auto maybe_lang = get_attribute(HTML::AttributeNames::lang);
            if (maybe_lang.has_value())
                return maybe_lang.release_value();
        }

        // 3. If the node's parent is a shadow root
        //      Use the language of that shadow root's host.
        if (auto const* parent = parent_element()) {
            if (parent->is_shadow_root())
                return parent->shadow_root()->host()->lang();
        }

        // 4. If the node's parent element is not null
        //      Use the language of that parent element.
        if (auto const* parent = parent_element())
            return parent->lang();

        // 5. Otherwise
        //      - If there is a pragma-set default language set, then that is the language of the node.
        if (document().pragma_set_default_language().has_value()) {
            return document().pragma_set_default_language();
        }

        //      - If there is no pragma-set default language set, then language information from a higher-level protocol (such as HTTP),
        if (document().http_content_language().has_value()) {
            return document().http_content_language();
        }

        //        if any, must be used as the final fallback language instead.
        //      - In the absence of any such language information, and in cases where the higher-level protocol reports multiple languages,
        //        the language of the node is unknown, and the corresponding language tag is the empty string.
        // Default locale sounds like a reasonable fallback here.
        return {};
    };

    // If the resulting value is the empty string, then it must be interpreted as meaning that the language of the node is explicitly unknown.
    auto maybe_lang = attempt_to_determine_lang_attribute();
    if (!maybe_lang.has_value() || maybe_lang->is_empty())
        return {};
    return maybe_lang.release_value();
}
}
