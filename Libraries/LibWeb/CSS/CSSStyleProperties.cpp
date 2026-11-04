/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <AK/Utf16StringBuilder.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/ExternalMemory.h>
#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/CustomPropertyData.h>
#include <LibWeb/CSS/CustomPropertyRegistration.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StyleSheetInvalidation.h>
#include <LibWeb/CSS/StyleSheetState.h>
#include <LibWeb/CSS/StyleValues/FilterStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/PendingSubstitutionStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShorthandStyleValue.h>
#include <LibWeb/CSS/StyleValues/UnresolvedStyleValue.h>
#include <LibWeb/CSS/Zoom.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/RenderDocument.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/BoxModelMetrics.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/SVG/TagNames.h>
#include <LibWebCommon/Infra/Strings.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSStyleProperties);

GC::Ref<CSSStyleProperties> CSSStyleProperties::create(Vector<StyleProperty> properties, OrderedHashMap<Utf16FlyString, StyleProperty> custom_properties)
{
    return create(RustDeclarationBlock { move(properties), move(custom_properties) });
}

GC::Ref<CSSStyleProperties> CSSStyleProperties::create(RustDeclarationBlock declarations)
{
    // https://drafts.csswg.org/cssom/#dom-cssstylerule-style
    // The style attribute must return a CSSStyleProperties object for the style rule, with the following properties:
    //     computed flag: Unset.
    //     readonly flag: Unset.
    //     declarations: The declared declarations in the rule, in specified order.
    //     parent CSS rule: The context object.
    //     owner node: Null.
    return GC::Heap::the().allocate<CSSStyleProperties>(Computed::No, Readonly::No, move(declarations), OptionalNone {});
}

GC::Ref<CSSStyleProperties> CSSStyleProperties::create_resolved_style(Optional<DOM::AbstractElement> element_reference)
{
    // https://drafts.csswg.org/cssom/#dom-window-getcomputedstyle
    // 6.  Return a live CSSStyleProperties object with the following properties:
    //     computed flag: Set.
    //     readonly flag: Set.
    //     declarations: decls.
    //     parent CSS rule: Null.
    //     owner node: obj.
    // AD-HOC: Rather than instantiate with a list of decls, they're generated on demand.
    return GC::Heap::the().allocate<CSSStyleProperties>(Computed::Yes, Readonly::Yes, RustDeclarationBlock { {}, {} }, move(element_reference));
}

GC::Ref<CSSStyleProperties> CSSStyleProperties::create_element_inline_style(DOM::AbstractElement element_reference)
{
    // https://drafts.csswg.org/cssom/#dom-elementcssinlinestyle-style
    // The style attribute must return a CSS declaration block object whose readonly flag is unset, whose parent CSS
    // rule is null, and whose owner node is the context object.
    return create_element_inline_style(move(element_reference), RustDeclarationBlock { {}, {} });
}

GC::Ref<CSSStyleProperties> CSSStyleProperties::create_element_inline_style(DOM::AbstractElement element_reference, RustDeclarationBlock declarations)
{
    return GC::Heap::the().allocate<CSSStyleProperties>(Computed::No, Readonly::No, move(declarations), move(element_reference));
}

CSSStyleProperties::CSSStyleProperties(Computed computed, Readonly readonly, RustDeclarationBlock declarations, Optional<DOM::AbstractElement> owner_node)
    : CSSStyleDeclaration(computed, readonly)
    , m_declarations(move(declarations))
{
    set_owner_node(move(owner_node));
}

CSSStyleProperties::~CSSStyleProperties() = default;

extern "C" void web_css_style_properties_create(void* vm, void* declaration);

void CSSStyleProperties::create_owner()
{
    web_css_style_properties_create(JS::HostABI::vm_to_abi(JS::VM::the()), static_cast<GC::Cell*>(this));
}

size_t CSSStyleProperties::external_memory_size() const
{
    auto size = JS::saturating_add_external_memory_size(Base::external_memory_size(), m_declarations.external_memory_size());
    return JS::saturating_add_external_memory_size(size, m_cached_computed_custom_property_names.capacity() * sizeof(Utf16FlyString));
}

// https://drafts.csswg.org/cssom/#dom-window-getcomputedstyle
static bool element_exposes_computed_style(DOM::Element const& element)
{
    // NB: This is a partial enforcement of step 5:
    // If [...] elt is connected, part of the flat tree, and its shadow-including root has a browsing context which
    // either doesn't have a browsing context container, or whose browsing context container is being rendered.
    if (!element.is_connected())
        return false;
    if (!element.shadow_including_root().document().browsing_context())
        return false;
    // FIXME: Check if the element is part of the flat tree.
    // FIXME: Check that the browsing context either doesn't have a browsing context container, or that its
    //        browsing context container is being rendered.
    return true;
}

Vector<Utf16FlyString> const& CSSStyleProperties::computed_custom_property_names() const
{
    auto const& element = *owner_node();
    auto& document = element.document();
    document.update_style_for_element(element);
    auto data = element.custom_property_data();
    auto registration_generation = document.custom_property_registration_generation();
    if (m_cached_computed_custom_property_document.ptr().ptr() == &document
        && m_cached_computed_custom_property_data == data
        && m_cached_custom_property_registration_generation == registration_generation)
        return m_cached_computed_custom_property_names;

    HashTable<Utf16FlyString> names;
    if (data) {
        data->for_each_property([&](Utf16FlyString const& name, StyleProperty const& property) {
            if (!property.value->is_guaranteed_invalid())
                names.set(name);
        });
    }
    document.for_each_registered_custom_property([&](CustomPropertyRegistration const& registration) {
        if (registration.initial_value && !registration.initial_value->is_guaranteed_invalid())
            names.set(registration.property_name);
    });
    m_cached_computed_custom_property_names.clear_with_capacity();
    m_cached_computed_custom_property_names.ensure_capacity(names.size());
    for (auto const& name : names)
        m_cached_computed_custom_property_names.unchecked_append(name);
    quick_sort(m_cached_computed_custom_property_names);
    m_cached_computed_custom_property_document = document;
    m_cached_computed_custom_property_data = move(data);
    m_cached_custom_property_registration_generation = registration_generation;
    return m_cached_computed_custom_property_names;
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-length
size_t CSSStyleProperties::length() const
{
    // The length attribute must return the number of CSS declarations in the declarations.
    if (is_computed()) {
        if (!owner_node().has_value())
            return 0;
        if (!element_exposes_computed_style(owner_node()->element()))
            return 0;
        return number_of_longhand_properties + computed_custom_property_names().size();
    }

    return properties().size() + custom_properties().size();
}

Utf16String CSSStyleProperties::item(size_t index) const
{
    // The item(index) method must return the property name of the CSS declaration at position index.
    // If there is no indexth object in the collection, then the method must return the empty string.
    auto custom_properties_count = custom_properties().size();

    if (index >= length())
        return {};

    if (is_computed()) {
        if (index >= number_of_longhand_properties)
            return computed_custom_property_names()[index - number_of_longhand_properties].to_utf16_string();
        auto property_id = static_cast<PropertyID>(index + to_underlying(first_longhand_property_id));
        return string_from_property_id(property_id).to_utf16_string();
    }

    if (index < custom_properties_count)
        return custom_properties().keys()[index].to_utf16_string();

    return string_from_property_id(properties()[index - custom_properties_count].property_id).to_utf16_string();
}

Optional<StyleProperty> CSSStyleProperties::get_property(PropertyID property_id) const
{
    return get_property_internal(PropertyNameAndID::from_id(property_id));
}

Optional<StyleProperty const&> CSSStyleProperties::custom_property(Utf16FlyString const& custom_property_name) const
{
    if (is_computed()) {
        if (!owner_node().has_value())
            return {};

        auto& element = owner_node()->element();
        auto pseudo_element = owner_node()->pseudo_element();

        element.document().update_style_for_element(*owner_node());

        auto data = element.custom_property_data(pseudo_element);
        if (!data)
            return {};

        return data->get(custom_property_name);
    }

    return custom_properties().get(custom_property_name);
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-setproperty
WebIDL::ExceptionOr<void> CSSStyleProperties::set_property(Utf16FlyString const& property_name, Utf16View value, Utf16View priority)
{
    // 1. If the computed flag is set, then throw a NoModificationAllowedError exception.
    if (is_computed())
        return WebIDL::NoModificationAllowedError::create("Cannot modify properties in result of getComputedStyle()"_utf16);

    // 2. If property is not a custom property, follow these substeps:
    //    1. Let property be property converted to ASCII lowercase.
    //    2. If property is not a case-sensitive match for a supported CSS property, then return.
    // NB: This is handled inside PropertyNameAndID::from_string().
    auto property = PropertyNameAndID::from_name(property_name);
    if (!property.has_value())
        return {};

    if (value.is_empty() && is_readonly())
        return WebIDL::NoModificationAllowedError::create("Cannot remove property: CSSStyleProperties is read-only."_utf16);

    // NB: The remaining steps are implemented in set_property_internal().
    return set_property_internal(property.release_value(), value, priority);
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-setproperty
WebIDL::ExceptionOr<void> CSSStyleProperties::set_property_internal(PropertyNameAndID const& property, Utf16View value, Utf16View priority)
{
    // NB: Steps 1 and 2 only apply to the IDL method that invokes this.

    // 3. If value is the empty string, invoke removeProperty() with property as argument and return.
    if (value.is_empty()) {
        auto removed_value = MUST(remove_property_internal(property));
        (void)removed_value;
        return {};
    }

    // 4. If priority is not the empty string and is not an ASCII case-insensitive match for the string "important", then return.
    if (!priority.is_empty() && !priority.equals_ignoring_ascii_case(u"important"sv))
        return {};

    // 5. Let component value list be the result of parsing value for property property.
    auto component_value_list = owner_node().has_value()
        ? parse_css_value(Parser::ParsingParams { owner_node()->element().document() }, value, property.id())
        : parse_css_value(Parser::ParsingParams {}, value, property.id());

    // 6. If component value list is null, then return.
    if (!component_value_list)
        return {};

    prepare_to_update_style_attribute();

    // NB: Steps 7 to 9 are implemented in RustDeclarationBlock::set_expanding_shorthands().
    auto important = !priority.is_empty() ? Important::Yes : Important::No;
    bool updated = false;
    if (property.is_custom_property()) {
        StyleProperty style_property {
            .important = important,
            .property_id = property.id(),
            .value = component_value_list.release_nonnull(),
        };
        auto existing_property = custom_property(property.name());
        if (!existing_property.has_value() || existing_property->important != important || *existing_property->value != *style_property.value) {
            m_declarations.set_custom(property.name(), style_property);
            updated = true;
        }
    } else {
        updated = m_declarations.set_expanding_shorthands(property.id(), *component_value_list, important);
    }

    // 10. If updated is true, update style attribute for the CSS declaration block.
    if (updated) {
        update_style_attribute();

        // Non-standard: Invalidate style for the owners of our containing sheet, if any.
        invalidate_owners();
    }

    return {};
}

WebIDL::ExceptionOr<void> CSSStyleProperties::set_property(PropertyID property_id, Utf16View css_text, Utf16View priority)
{
    VERIFY(!is_computed());
    return set_property_internal(PropertyNameAndID::from_id(property_id), css_text, priority);
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-getpropertyvalue
Utf16String CSSStyleProperties::get_property_value(Utf16FlyString const& property_name) const
{
    if (auto property = PropertyNameAndID::from_name(property_name); property.has_value()) {
        if (is_computed() && !property->is_custom_property()) {
            if (auto serialized = serialized_computed_value_from_stored_handle(property->id()); serialized.has_value())
                return serialized.release_value();
        }
        if (auto style_property = get_property_internal(*property); style_property.has_value())
            return style_property->value->to_utf16_string(is_computed() ? SerializationMode::ResolvedValue : SerializationMode::Normal);
    }

    return {};
}
// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-getpropertypriority
Utf16String CSSStyleProperties::get_property_priority(Utf16FlyString const& property_name) const
{
    auto property = PropertyNameAndID::from_name(property_name);
    if (!property.has_value())
        return {};
    if (property->is_custom_property()) {
        auto maybe_custom_property = custom_property(property_name);
        if (!maybe_custom_property.has_value())
            return {};
        return maybe_custom_property.value().important == Important::Yes ? "important"_utf16 : Utf16String {};
    }
    auto maybe_property = get_property_internal(property.value());
    if (!maybe_property.has_value())
        return {};
    return maybe_property->important == Important::Yes ? "important"_utf16 : Utf16String {};
}

bool CSSStyleProperties::has_property(PropertyNameAndID const& property) const
{
    return get_property_internal(property).has_value();
}

bool CSSStyleProperties::has_property(PropertyID property_id) const
{
    return has_property(PropertyNameAndID::from_id(property_id));
}

RefPtr<StyleValue const> CSSStyleProperties::get_property_style_value(PropertyNameAndID const& property) const
{
    if (auto style_property = get_property_internal(property); style_property.has_value())
        return style_property->value;
    return nullptr;
}

RefPtr<StyleValue const> CSSStyleProperties::get_property_style_value(PropertyID property_id) const
{
    return get_property_style_value(PropertyNameAndID::from_id(property_id));
}

WebIDL::ExceptionOr<void> CSSStyleProperties::set_property_style_value(PropertyNameAndID const& property, NonnullRefPtr<StyleValue const> style_value)
{
    if (is_computed()) {
        return WebIDL::NoModificationAllowedError::create("Cannot modify properties in result of getComputedStyle()"_utf16);
    }

    if (property.is_custom_property()) {
        prepare_to_update_style_attribute();

        m_declarations.remove_custom(property.name());
        m_declarations.set_custom(property.name(),
            StyleProperty {
                Important::No,
                PropertyID::Custom,
                style_value });

        update_style_attribute();
        invalidate_owners();

        return {};
    }

    // FIXME: This should have been rejected earlier, but property_accepts_type() is too basic for what we need.
    if (property_is_positional_value_list_shorthand(property.id())
        && !style_value->is_shorthand()
        && !style_value->is_unresolved()
        && !style_value->is_pending_substitution()
        && !style_value->is_guaranteed_invalid()
        && !style_value->is_css_wide_keyword()) {
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, Utf16String::formatted("Setting {} to '{}' is not allowed.", property.name(), style_value->to_string(SerializationMode::Normal)) };
    }

    if (first_is_one_of(property.id(), PropertyID::BackdropFilter, PropertyID::Filter)
        && style_value->is_value_list()
        && !is_filter_style_value_list(*style_value)) {
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, Utf16String::formatted("Setting {} to '{}' is not allowed.", property.name(), style_value->to_string(SerializationMode::Normal)) };
    }

    prepare_to_update_style_attribute();

    m_declarations.append_expanding_shorthands(property.id(), style_value, Important::No);

    update_style_attribute();
    invalidate_owners();

    return {};
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-getpropertyvalue
Optional<StyleProperty> CSSStyleProperties::get_property_internal(PropertyNameAndID const& property) const
{
    // NB: This is our own method to get a StyleProperty, but following the algorithm for getPropertyValue() which
    //     returns a String. (This way, we can use the same logic in other places.) That's why the spec steps talk
    //     about strings and then we do something different.

    // 1. If property is not a custom property, follow these substeps:
    if (!property.is_custom_property()) {
        // 1. Let property be property converted to ASCII lowercase.
        // NB: Done already by PropertyNameAndID.

        // 2. If property is a shorthand property, then follow these substeps:
        if (property_is_shorthand(property.id())) {
            // 1. Let list be a new empty array.
            Vector<ValueComparingNonnullRefPtr<StyleValue const>> list;
            Vector<void const*> longhand_values;
            Optional<Important> last_important_flag;

            // 2. For each longhand property longhand that property maps to, in canonical order, follow these substeps:
            for (auto longhand_property_id : longhands_for_shorthand(property.id())) {
                // 1. If longhand is a case-sensitive match for a property name of a CSS declaration in the declarations,
                //    let declaration be that CSS declaration, or null otherwise.
                auto declaration = get_property_internal(PropertyNameAndID::from_id(longhand_property_id));

                // 2. If declaration is null, then return the empty string.
                if (!declaration.has_value())
                    return {};

                // 3. Append the declaration to list.
                longhand_values.append(declaration->value->rust_style_value_data());
                list.append(declaration->value);

                if (last_important_flag.has_value() && declaration->important != *last_important_flag)
                    return {};
                last_important_flag = declaration->important;
            }

            // 3. If important flags of all declarations in list are same, then return the serialization of list.
            auto const* shorthand_value = Parser::ValueParserFFI::rust_shorthand_value_of_longhands(to_underlying(property.id()), longhand_values.data(), longhand_values.size());
            if (!shorthand_value)
                return {};
            return StyleProperty {
                .important = last_important_flag.value(),
                .property_id = property.id(),
                .value = StyleValue::adopt_rust_style_value_data(static_cast<StyleValueFFI::StyleValueData const*>(shorthand_value)),
            };

            // 4. Return the empty string.
            // NOTE: This is handled by the loop.
        }
    }

    // 2. If property is a case-sensitive match for a property name of a CSS declaration in the declarations, then
    //    return the result of invoking serialize a CSS value of that declaration.
    // 3. Return the empty string.
    return get_direct_property(property);
}

// Install the style engine's record for a synthetic pseudo-element a CSSOM read asks for, settled against its
// element's installed record as a style update settles it. A pseudo-element the engine gives no answer for keeps the
// record it holds.
static void install_engine_pseudo_element_style(Layout::BegunRead const& read, DOM::AbstractElement target)
{
    auto pseudo_element = *target.pseudo_element();
    auto& element = target.element();
    auto& style_engine = element.document().style_computer().style_engine();
    auto answer = StyleEngineFFI::style_engine_answer_pseudo_element_record_demand(style_engine.host(), &read, element.style_node_id().value(), StyleEngine::PseudoElementRecordDemand::CssomRead, *StyleEngine::demanded_pseudo_element(pseudo_element));
    if (!answer.is_absent && answer.record.style_record == 0)
        return;
    StyleRecordID record { answer.record.style_record };
    element.set_computed_style(read, pseudo_element, InstalledStyle { answer.record.host_record });
    // A pseudo-element that generates no box holds the environment its rules declare, if any rule styles it.
    auto environment = !!record ? InstalledStyle { answer.record.host_record }.custom_property_environment() : answer.custom_property_environment;
    if (!!record || environment != 0)
        element.install_engine_pseudo_element_custom_property_data(read, pseudo_element, environment);
    else
        element.set_custom_property_data(pseudo_element, nullptr);
    if (!!record)
        element.document().style_computer().compose_installed_engine_record(read, target, {});
}

static void ensure_pseudo_element_style_for_cssom(Layout::BegunRead const& read, DOM::AbstractElement abstract_element)
{
    auto pseudo_element = abstract_element.pseudo_element();
    if (!pseudo_element.has_value())
        return;
    if (!is_synthetic_pseudo_element(*pseudo_element))
        return;
    // A style update keeps current the records of the pseudo-elements an element generates boxes for: its ::before,
    // ::after and ::first-letter, and a list item's ::marker. A read settles any other kind again.
    auto is_kept_current = [&] {
        switch (*pseudo_element) {
        case PseudoElement::Before:
        case PseudoElement::After:
        case PseudoElement::FirstLetter:
            return true;
        case PseudoElement::Marker: {
            auto element_style = abstract_element.element().computed_style();
            return element_style && element_style->display().is_list_item();
        }
        default:
            return false;
        }
    };
    if (is_kept_current() && abstract_element.computed_style())
        return;

    auto& document = abstract_element.document();
    document.begin_style_stabilization_epoch();
    ScopeGuard end_stabilization_epoch = [&] {
        document.end_style_stabilization_epoch();
    };
    // A highlight pseudo-element inherits from its parent element's, which is only kept current near an active
    // highlight, so the chain is computed outermost first.
    if (is_highlight_pseudo_element(*pseudo_element)) {
        GC::ConservativeVector<DOM::AbstractElement> ancestors;
        for (auto ancestor = abstract_element.element().element_to_inherit_style_from({}); ancestor; ancestor = ancestor->element_to_inherit_style_from({}))
            ancestors.append({ *ancestor, pseudo_element });
        for (auto& ancestor : ancestors.in_reverse())
            install_engine_pseudo_element_style(read, ancestor);
    }
    install_engine_pseudo_element_style(read, abstract_element);
}

static bool is_min_or_max_size_property(PropertyID property_id)
{
    return first_is_one_of(property_id,
        PropertyID::MaxBlockSize, PropertyID::MaxHeight, PropertyID::MaxInlineSize, PropertyID::MaxWidth,
        PropertyID::MinBlockSize, PropertyID::MinHeight, PropertyID::MinInlineSize, PropertyID::MinWidth);
}

static bool min_or_max_size_holds_anchor_function(ComputedValues const& style, PropertyID property_id)
{
    // NB: computed_style_value() maps a logical alias to its physical property.
    return style.computed_style_value(property_id)->contains_anchor_function();
}

// NB: An element without a layout node may still generate a box whose layout is not up to date yet, so this only
//     looks at whether the element's style places it outside the box tree.
static bool element_generates_no_box(DOM::AbstractElement abstract_element, ComputedValues const& style)
{
    if (style.display().is_none() || style.display().is_contents())
        return true;
    // NB: content: none keeps ::before, ::after and ::marker out of the box tree, as normal (which computes to none
    //     for them) does for ::before and ::after.
    if (auto pseudo_element = abstract_element.pseudo_element(); pseudo_element.has_value()) {
        bool is_before_or_after = first_is_one_of(*pseudo_element, PseudoElement::Before, PseudoElement::After);
        if (is_before_or_after && style.content_is_normal())
            return true;
        auto content = style.computed_content();
        if ((is_before_or_after || *pseudo_element == PseudoElement::Marker) && content->is_keyword() && content->to_keyword() == Keyword::None)
            return true;
    }
    auto const& installed_style = abstract_element.installed_style();
    return installed_style && has_flag(installed_style.dependency_flags(), StyleRecordDependencyFlag::InDisplayNoneSubtree);
}

// Brings style (and, when the property needs it, layout) up to date for computed-style property
// access, and returns the layout node to read used values from (may be null). An empty Optional
// means the element cannot expose computed style at all (disconnected, or no browsing context).
static Optional<Layout::NodeWithStyle*> prepare_computed_style_and_layout_for_property(DOM::AbstractElement abstract_element, PropertyID property_id)
{
    if (!element_exposes_computed_style(abstract_element.element()))
        return {};

    Layout::ForcedReadScope read { abstract_element.document() };

    // NB: We grab the layout node before deciding whether update_layout() is needed.
    //     For properties that don't need layout or a layout node (the else branch below),
    //     we skip update_layout() entirely and use whatever layout node already exists.
    //     For the other paths, we call update_layout() and re-fetch below.
    Layout::NodeWithStyle* layout_node = abstract_element.unsafe_layout_node(read);

    // Determine what work is needed for this property:
    // 1. Properties that need layout computation (used values) - always run update_layout()
    // 2. Properties that need a layout node for special resolution - ensure layout node exists
    // 3. Everything else - just update_style() and return computed value
    bool const needs_layout = property_needs_layout_for_getcomputedstyle(property_id);
    bool const needs_layout_node = property_needs_layout_node_for_resolved_value(property_id);

    if (needs_layout || needs_layout_node) {
        // Properties that need layout computation or layout node for special resolution
        // always need update_layout() to ensure both style and layout tree are up to date.
        abstract_element.document().update_layout_if_needed_for_node(abstract_element.element(), DOM::UpdateLayoutReason::ResolvedCSSStyleDeclarationProperty);
        layout_node = abstract_element.layout_node(read);
    }
    // Ensure styles are up to date. update_layout()/update_style() skip display:none subtrees,
    // so the leaf and its inheritance ancestors may still be stale at this point.
    // NB: Only the style record's presence and its display:none-subtree bit matter here, so probe
    //     those directly instead of materializing a full style record view.
    auto const& style = abstract_element.installed_style();
    // NB: An element moved below a display:none element holds the record it had where it stood, and its box until
    //     layout next runs.
    bool const style_is_in_display_none_subtree = style
        && ((!layout_node && has_flag(style.dependency_flags(), StyleRecordDependencyFlag::InDisplayNoneSubtree))
            || ((!layout_node || !abstract_element.document().layout_is_up_to_date())
                && abstract_element.element().has_inclusive_ancestor_with_display_none_ignoring_animations()));
    if (!style || style_is_in_display_none_subtree)
        abstract_element.document().update_style_for_element(abstract_element);
    else
        abstract_element.document().update_style_for_element(abstract_element, DOM::Document::StyleUpdateMode::OnlyIfNeeded);
    ensure_pseudo_element_style_for_cssom(read, abstract_element);

    // NB: Only layout resolves the anchor-size() functions of a min or max sizing property against an anchor box.
    if (!needs_layout && !needs_layout_node && is_min_or_max_size_property(property_id)) {
        if (auto style = abstract_element.computed_style(); style && min_or_max_size_holds_anchor_function(*style, property_id)) {
            abstract_element.document().update_layout_if_needed_for_node(abstract_element.element(), DOM::UpdateLayoutReason::ResolvedCSSStyleDeclarationProperty);
            layout_node = abstract_element.layout_node(read);
        }
    }

    // Container queries and container-relative units need layout to resolve. Avoid forcing layout for every
    // getComputedStyle() call; only elements that actually depend on a query container need the post-layout style.
    bool style_or_inheritance_ancestor_depends_on_size_container_query = abstract_element.element().style_depends_on_size_container_query();
    for (auto ancestor = abstract_element.element_to_inherit_style_from();
        ancestor.has_value() && !style_or_inheritance_ancestor_depends_on_size_container_query;
        ancestor = ancestor->element_to_inherit_style_from()) {
        style_or_inheritance_ancestor_depends_on_size_container_query = ancestor->element().style_depends_on_size_container_query();
    }
    bool const needs_layout_for_container_queries = style_or_inheritance_ancestor_depends_on_size_container_query
        && !abstract_element.document().layout_is_up_to_date();
    if (needs_layout_for_container_queries) {
        abstract_element.document().update_layout_if_needed_for_node(abstract_element.element(), DOM::UpdateLayoutReason::ResolvedCSSStyleDeclarationProperty);
        layout_node = abstract_element.layout_node(read);
        // NB: The style update of the layout skips display:none subtrees, so a style read below one is answered
        //     again against the container sizes the layout settled.
        if (auto const& settled_style = abstract_element.installed_style(); !layout_node && settled_style
            && has_flag(settled_style.dependency_flags(), StyleRecordDependencyFlag::InDisplayNoneSubtree))
            abstract_element.document().update_style_for_element(abstract_element);
        // A synthetic pseudo which is not rendered is not part of the layout-driven pseudo
        // recomputation above. Refresh its CSSOM-only style against the settled container size.
        ensure_pseudo_element_style_for_cssom(read, abstract_element);
    }

    if (auto pseudo_element = abstract_element.pseudo_element(); layout_node && pseudo_element.has_value()) {
        auto pseudo_style = abstract_element.element().computed_style(*pseudo_element);
        if (!pseudo_style || pseudo_style->display().is_contents())
            layout_node = nullptr;
    }

    return layout_node;
}

Optional<RefPtr<StyleValue const>> CSSStyleProperties::resolved_value_read_from_computed_style(DOM::AbstractElement abstract_element, PropertyID property_id)
{
    // Only properties whose resolved value is the plain computed value qualify: no used-value substitution (see
    // property_needs_layout_for_getcomputedstyle() and property_needs_layout_node_for_resolved_value()): No color or
    // shadow resolution, no logical alias mapping, and no shorthand reconstruction. get_direct_property() and
    // style_value_for_computed_property() enumerate the properties that need such treatment.
    switch (property_id) {
    case PropertyID::Display:
    case PropertyID::FontFamily:
    case PropertyID::FontSize:
    case PropertyID::FontStyle:
    case PropertyID::FontWeight:
    case PropertyID::TextAlign:
    case PropertyID::TextDecorationLine:
    case PropertyID::TextWrapMode:
    case PropertyID::WhiteSpaceCollapse:
        break;
    default:
        return OptionalNone {};
    }

    auto prepared = prepare_computed_style_and_layout_for_property(abstract_element, property_id);
    if (!prepared.has_value())
        return RefPtr<StyleValue const> {};

    if (auto style = abstract_element.computed_style()) {
        auto value = style->computed_style_value(property_id);
        if (value)
            value = unscaled_by_zoom(property_id, value.release_nonnull(), style->effective_zoom());
        return value;
    }

    // Only a synthetic pseudo-element with no matching rules lacks a style record after the preparation above; its
    // transient style needs the full declaration path.
    return OptionalNone {};
}

Optional<StyleProperty> CSSStyleProperties::get_direct_property(PropertyNameAndID const& property_name_and_id) const
{
    auto const property_id = property_name_and_id.id();

    if (is_computed()) {
        if (!owner_node().has_value())
            return {};

        auto abstract_element = *owner_node();
        Layout::ForcedReadScope read { abstract_element.document() };

        auto maybe_layout_node = prepare_computed_style_and_layout_for_property(abstract_element, property_id);
        if (!maybe_layout_node.has_value())
            return {};
        auto* layout_node = *maybe_layout_node;

        // FIXME: Somehow get custom properties if there's no layout node.
        if (property_name_and_id.is_custom_property()) {
            if (auto maybe_value = abstract_element.get_custom_property(property_name_and_id.name())) {
                return StyleProperty {
                    .property_id = property_id,
                    .value = maybe_value.release_nonnull(),
                };
            }
            // Pseudo-elements may have no own custom-property data if the matching rule targeted the originating
            // element rather than the pseudo-element itself (for example `::slotted(...)`).
            // In that case, getComputedStyle(..., "::before") still needs to expose inherited custom properties from
            // the originating element.
            if (abstract_element.pseudo_element().has_value()) {
                if (auto inherit_from = abstract_element.element_to_inherit_style_from(); inherit_from.has_value()) {
                    if (auto maybe_value = inherit_from->get_custom_property(property_name_and_id.name())) {
                        return StyleProperty {
                            .property_id = property_id,
                            .value = maybe_value.release_nonnull(),
                        };
                    }
                }
            }
            if (auto registration = abstract_element.document().get_registered_custom_property(property_name_and_id.name()); registration.has_value() && registration->initial_value) {
                return StyleProperty {
                    .property_id = property_id,
                    .value = compute_registered_custom_property_initial_value(abstract_element.document(), *registration),
                };
            }

            return {};
        }

        if (!layout_node) {
            auto style_record = abstract_element.computed_style();
            RefPtr<ComputedValues const> transient_style;
            if (!style_record) {
                // A pseudo-element without a box has no durable style: a synthetic one without matching rules, one no
                // element in the shadow tree backs, or a named view transition one. The style engine derives the one
                // it would have for this read alone; a read it answers with none has no value.
                auto& style_computer = abstract_element.document().style_computer();
                if (auto pseudo_element = abstract_element.pseudo_element(); pseudo_element.has_value()) {
                    if (auto demanded_pseudo_element = StyleEngine::demanded_pseudo_element(*pseudo_element); demanded_pseudo_element.has_value())
                        transient_style = style_computer.engine_transient_pseudo_element_style(read, abstract_element.element(), *demanded_pseudo_element);
                }
                if (!transient_style)
                    return {};
            }
            auto const* computed_values = style_record ? &*style_record : transient_style.ptr();
            VERIFY(computed_values);

            Array<void const*, to_underlying(StyleGroupIndex::Count)> style_groups;
            for (size_t group = 0; group < style_groups.size(); ++group)
                style_groups[group] = computed_values->style_group_payload(static_cast<StyleGroupIndex>(group));
            auto computed_value = [](void* context, u16 computed_property_id) -> StyleValueFFI::StyleValueData const* {
                auto value = static_cast<ComputedValues const*>(context)->computed_style_value(static_cast<PropertyID>(computed_property_id));
                VERIFY(value);
                return StyleValueFFI::rust_style_value_retain(value->rust_style_value_data());
            };
            StyleValueFFI::FfiResolvedValueStyle resolved_value_style {
                .groups = style_groups.data(),
                .writing_mode = to_underlying(computed_values->writing_mode()),
                .direction = to_underlying(computed_values->direction()),
                .generates_no_box = element_generates_no_box(abstract_element, *computed_values),
                .computed_value_context = const_cast<ComputedValues*>(computed_values),
                .computed_value = computed_value,
            };
            return StyleProperty {
                .property_id = property_id,
                .value = unscaled_by_zoom(property_id,
                    StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_resolved_value_of_style(&resolved_value_style, to_underlying(property_id))),
                    computed_values->effective_zoom()),
            };
        }

        auto value = style_value_for_computed_property(*layout_node, property_id);
        if (!value)
            return {};
        return StyleProperty {
            .property_id = property_id,
            .value = unscaled_by_zoom(property_id, value.release_nonnull(), effective_zoom_of(layout_node->inherited_set_values())),
        };
    }

    if (property_name_and_id.is_custom_property())
        return custom_property(property_name_and_id.name()).copy();

    for (auto const& property : properties()) {
        if (property.property_id == property_id)
            return property;
    }
    return {};
}

// The properties eligible for serializing straight from the stored Rust style value handle:
// their computed value is kept as a handle (see ComputedValues::stored_style_value_handle()),
// and their resolved value is the computed value, except for the guards applied in
// serialized_computed_value_from_stored_handle() below.
static bool property_computed_value_may_be_stored_as_style_value_handle(PropertyID property_id)
{
    switch (property_id) {
    case PropertyID::Cx:
    case PropertyID::Cy:
    case PropertyID::D:
    case PropertyID::GridAutoColumns:
    case PropertyID::GridAutoRows:
    case PropertyID::GridColumnEnd:
    case PropertyID::GridColumnStart:
    case PropertyID::GridRowEnd:
    case PropertyID::GridRowStart:
    case PropertyID::GridTemplateAreas:
    case PropertyID::GridTemplateColumns:
    case PropertyID::GridTemplateRows:
    case PropertyID::LetterSpacing:
    case PropertyID::R:
    case PropertyID::Rx:
    case PropertyID::Ry:
    case PropertyID::WordSpacing:
    case PropertyID::X:
    case PropertyID::Y:
        return true;
    default:
        return false;
    }
}

// Serialize native value data without constructing a C++ style value wrapper.
static Utf16String serialize_style_value_handle(RustStyleValueHandle const& handle, SerializationMode mode)
{
    auto text = StyleValueFFI::rust_style_value_serialize(handle.data(), to_underlying(mode));
    VERIFY(text);
    return Utf16String::adopt_raw(text);
}

Optional<Utf16String> CSSStyleProperties::serialized_computed_value_from_stored_handle(PropertyID property_id) const
{
    // Fast path for getComputedStyle() serialization: when the computed value already lives in the
    // style record as a Rust style value, serialize it straight from the stored handle instead of
    // reconstructing a style value wrapper for it. Every property that needs used-value or color
    // resolution stays on the value-building path; an empty Optional means "not handled here".
    VERIFY(is_computed());
    if (!property_computed_value_may_be_stored_as_style_value_handle(property_id))
        return {};
    if (!owner_node().has_value())
        return {};
    auto abstract_element = *owner_node();

    auto maybe_layout_node = prepare_computed_style_and_layout_for_property(abstract_element, property_id);
    if (!maybe_layout_node.has_value())
        return {};
    auto* layout_node = *maybe_layout_node;

    auto style = abstract_element.computed_style();
    if (!style)
        return {};

    // NB: A value of an element with a zoom must be unscaled, which the value path does.
    if (style->effective_zoom() != 1)
        return {};

    // letter-spacing: a used value of zero resolves to `normal`; leave that to the value path.
    // https://drafts.csswg.org/css-text-4/#letter-spacing-property
    if (property_id == PropertyID::LetterSpacing && layout_node && layout_node->letter_spacing() == 0)
        return {};

    // grid-template-columns/rows: with a laid-out grid box, the resolved value reflects the used
    // track sizes rather than the computed track definitions.
    // https://www.w3.org/TR/css-grid-2/#resolved-track-list-standalone
    if (property_id == PropertyID::GridTemplateColumns || property_id == PropertyID::GridTemplateRows) {
        if (layout_node && Painting::has_committed_box(*layout_node)) {
            if (auto value = Painting::used_value_for_grid_template(*layout_node, property_id))
                return serialize_style_value_handle(value, SerializationMode::ResolvedValue);
        }
    }

    auto const* handle = style->stored_style_value_handle(property_id);
    if (!handle)
        return {};
    return serialize_style_value_handle(*handle, SerializationMode::ResolvedValue);
}

// https://svgwg.org/svg2-draft/geometry.html#Sizing
// The CSS width and height properties are used for sizing some SVG elements. Specifically, they are used to size
// ‘rect’, ‘svg’, ‘image’ and ‘foreignObject’.
static bool svg_sizing_properties_apply(DOM::Element const& element)
{
    if (!element.is_svg_element())
        return true;
    return element.local_name().is_one_of(SVG::TagNames::rect, SVG::TagNames::svg, SVG::TagNames::image, SVG::TagNames::foreignObject);
}

RefPtr<StyleValue const> CSSStyleProperties::style_value_for_computed_property(Layout::NodeWithStyle const& layout_node, PropertyID property_id) const
{
    if (!owner_node().has_value()) {
        dbgln_if(LIBWEB_CSS_DEBUG, "Computed style for CSSStyleProperties without owner node was requested");
        return nullptr;
    }

    // https://drafts.csswg.org/css-tables-3/#table-wrapper-box
    // The computed values of properties position, float, margin-*, top, right, bottom, and left on the table are used
    // on the table-wrapper box and not the table grid box;
    // NB: So a table's insets and margins, computed and used, are read from its table wrapper.
    auto const& principal_layout_node = [&]() -> Layout::NodeWithStyle const& {
        if (layout_node.display().is_table_inside()) {
            if (auto const* parent = layout_node.parent(); parent && parent->is_table_wrapper())
                return *parent;
        }
        return layout_node;
    }();
    // NB: Only the resolved values of the margins and the insets depend on the containing block. A logical alias may be
    //     one of them, as it only maps to its physical property when the value is resolved.
    bool may_be_margin_or_inset = property_is_logical_alias(property_id) || first_is_one_of(property_id, PropertyID::MarginTop, PropertyID::MarginRight, PropertyID::MarginBottom, PropertyID::MarginLeft, PropertyID::Top, PropertyID::Right, PropertyID::Bottom, PropertyID::Left);
    auto const* containing_block = may_be_margin_or_inset ? principal_layout_node.containing_block() : nullptr;
    // NB: Only the insets of a sticky box are relative to a scroll container.
    Layout::NodeWithStyle const* scroll_container = nullptr;
    if (may_be_margin_or_inset && principal_layout_node.is_sticky_position()) {
        scroll_container = containing_block;
        while (scroll_container && !scroll_container->is_scroll_container())
            scroll_container = scroll_container->containing_block();
    }

    auto abstract_element = *owner_node();
    auto const* element_style = abstract_element.element().style_record_groups(abstract_element.pseudo_element());
    VERIFY(element_style);
    auto computed_value = [](void* context, u16 computed_property_id) -> StyleValueFFI::StyleValueData const* {
        auto style = static_cast<DOM::AbstractElement*>(context)->computed_style();
        VERIFY(style);
        auto value = style->computed_style_value(static_cast<PropertyID>(computed_property_id));
        VERIFY(value);
        return StyleValueFFI::rust_style_value_retain(value->rust_style_value_data());
    };
    StyleValueFFI::FfiResolvedValueBox resolved_value_box {
        .host = layout_node.document_host(),
        .node = Layout::Node::slot_id(&layout_node).index,
        .principal_node = Layout::Node::slot_id(&principal_layout_node).index,
        .has_containing_block = containing_block != nullptr,
        .containing_block = containing_block ? Layout::Node::slot_id(containing_block).index : 0,
        .has_scroll_container = scroll_container != nullptr,
        .scroll_container = scroll_container ? Layout::Node::slot_id(scroll_container).index : 0,
        .element_style = element_style,
        .svg_sizing_properties_apply = svg_sizing_properties_apply(abstract_element.element()),
        .computed_value_context = &abstract_element,
        .computed_value = computed_value,
    };
    return StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_resolved_value_of_box(&resolved_value_box, to_underlying(property_id)));
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-removeproperty
WebIDL::ExceptionOr<Utf16String> CSSStyleProperties::remove_property(Utf16FlyString const& property_name)
{
    // 1. If the readonly flag is set, then throw a NoModificationAllowedError exception.
    if (is_readonly())
        return WebIDL::NoModificationAllowedError::create("Cannot remove property: CSSStyleProperties is read-only."_utf16);

    return remove_property_internal(PropertyNameAndID::from_name(property_name));
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-removeproperty
WebIDL::ExceptionOr<Utf16String> CSSStyleProperties::remove_property_internal(Optional<PropertyNameAndID> const& property)
{
    VERIFY(!is_readonly());

    // 2. If property is not a custom property, let property be property converted to ASCII lowercase.
    // NB: Already done by creating a PropertyNameAndID.

    // NB: The spec doesn't reject invalid property names, it just lets them pass through.
    //     Attempting to remove a non-existent property is a no-op, so we can just skip over this section.
    Utf16String value;
    if (property.has_value()) {
        // 3. Let value be the return value of invoking getPropertyValue() with property as argument.
        // FIXME: Add an overload that takes PropertyNameAndID?
        value = get_property_value(property->name());

        prepare_to_update_style_attribute();

        // NB: Steps 4 to 6 are implemented in RustDeclarationBlock::remove_expanding_shorthands().
        auto removed = property->is_custom_property()
            ? m_declarations.remove_custom(property->name())
            : m_declarations.remove_expanding_shorthands(property->id());

        // 7. If removed is true, Update style attribute for the CSS declaration block.
        if (removed) {
            update_style_attribute();

            // Non-standard: Invalidate style for the owners of our containing sheet, if any.
            invalidate_owners();
        }
    }

    // 8. Return value.
    return value;
}

WebIDL::ExceptionOr<Utf16String> CSSStyleProperties::remove_property(PropertyID property_name)
{
    return remove_property_internal(PropertyNameAndID::from_id(property_name));
}

// https://drafts.csswg.org/cssom/#dom-cssstyleproperties-cssfloat
// https://www.w3.org/TR/cssom/#serialize-a-css-declaration-block
Utf16String CSSStyleProperties::serialized() const
{
    return Utf16String::adopt_raw(Parser::ValueParserFFI::rust_declaration_block_serialize(m_declarations.handle()));
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-csstext
WebIDL::ExceptionOr<void> CSSStyleProperties::set_css_text(Utf16View css_text)
{
    // 1. If the readonly flag is set, then throw a NoModificationAllowedError exception.
    if (is_readonly()) {
        return WebIDL::NoModificationAllowedError::create("Cannot modify properties: CSSStyleProperties is read-only."_utf16);
    }

    prepare_to_update_style_attribute();

    // 2. Empty the declarations.
    // 3. Parse the given value and, if the return value is not the empty list, insert the items in the list into the declarations, in specified order.
    set_declarations_from_text(css_text);

    // 4. Update style attribute for the CSS declaration block.
    update_style_attribute();

    // Non-standard: Invalidate style for the owners of our containing sheet, if any.
    invalidate_owners();

    return {};
}

void CSSStyleProperties::invalidate_owners()
{
    if (auto rule = parent_rule()) {
        if (auto sheet = rule->parent_style_sheet()) {
            record_style_rule_declarations_changed(*rule);

            // What a style rule's declarations say is published for the rule itself, so the scope's rule cache is
            // still good: the rules it holds have not moved. Any other rule kind can change what the cascade builds
            // from, and the sheet has to say so.
            if (rule->type() == CSSRule::Type::Style || rule->type() == CSSRule::Type::NestedDeclarations)
                return;

            sheet->invalidate_owners();
            if (rule->type() == CSSRule::Type::FontFace)
                sheet->synchronize_fonts_after_rule_change();
        }
    }
}

void CSSStyleProperties::set_declarations_from_text(Utf16View css_text)
{
    m_declarations.replace(RustDeclarationBlock { {}, {} });
    auto parsing_params = owner_node().has_value()
        ? Parser::ParsingParams(owner_node()->element().document())
        : Parser::ParsingParams();
    // NB: The declarations of an @position-try rule only accept some properties, which its rule context filters for.
    if (auto rule = parent_rule(); rule && rule->type() == CSSRule::Type::PositionTry)
        parsing_params.rule_context.append(Parser::RuleContext::AtPositionTry);
    else
        parsing_params.rule_context.append(Parser::RuleContext::Style);

    m_declarations.replace(parse_css_property_declaration_block(parsing_params, css_text));
}

void CSSStyleProperties::set_declarations_from(CSSStyleProperties const& source)
{
    m_declarations.replace(source.m_declarations);
}

}
