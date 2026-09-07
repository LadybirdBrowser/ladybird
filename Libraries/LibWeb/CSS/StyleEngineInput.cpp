/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <LibWeb/CSS/CSSPropertyRule.h>
#include <LibWeb/CSS/CSSStyleRule.h>
#include <LibWeb/CSS/Invalidation/LanguageInvalidator.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/SelectorMatching.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/CSS/StyleSheetImport.h>
#include <LibWeb/CSS/StyleSheetState.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/CustomElements/CustomStateSet.h>
#include <LibWeb/HTML/HTMLBRElement.h>
#include <LibWeb/HTML/HTMLHeadingElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLSlotElement.h>

namespace Web::CSS {

static void record_element_heading_level(DOM::Element&);
static void record_element_initial_features(DOM::Element&);
static void record_element_inline_style_properties(DOM::Element&);
static void record_heading_levels_in_subtree(DOM::Element&);
static Optional<StyleEngineFFI::FfiStateFact> state_fact_for(PseudoClass);
static StyleAtomID intern_id_or_class_atom(StyleEngine&, DOM::Element const&, Utf16FlyString const&);

static constexpr StyleNodeID no_style_node;
// Shadow trees get their own scopes with the shadow surface; today everything names the document.
static constexpr TreeScopeID document_tree_scope;

static bool has_pending_initial_features(DOM::Element const& element)
{
    return element.document().style_computer().style_engine().has_deferred_element_initial_features(element.style_node_id());
}

static StyleEngine* style_engine_for(DOM::Node& node)
{
    if (!node.is_connected())
        return nullptr;
    return &node.document().style_computer().style_engine();
}

// A relation is only nameable if the element on its other end already has an identity. Naming a
// node the engine has never seen would be worse than naming none: it would assert on a relation
// column that was never allocated.
static StyleNodeID identity_of(GC::Ptr<DOM::Element> element)
{
    if (!element)
        return no_style_node;
    return element->style_node_id();
}

// A shadow root's identity, minted on first use.
//
// The root is not an element and gets no style, but it is the parent its children's relations name.
// Giving it a real identity is what keeps a shadow tree inside the same relation columns as the
// document tree: a child combinator still stops at the root, and the subtree a `:host()` rule
// reaches is a subtree the engine can enumerate rather than a boundary it has to widen past.
static TreeScopeID tree_scope_of(DOM::Node&);

static StyleNodeID identity_of_shadow_root(DOM::ShadowRoot& shadow_root, StyleEngine& style_engine)
{
    if (shadow_root.style_node_id() == no_style_node) {
        shadow_root.set_style_node_id(style_engine.allocate_style_node());
        // A shadow root is a scope and a subtree at once. Naming the subtree is what lets a sheet
        // attached here be bounded by the tree it decides in, even when its rules dispatch on
        // nothing the engine can enumerate. It is named here rather than where a scope is numbered,
        // because numbering must not mint a place in the tree: a sheet detaching from a scope whose
        // root has already left would otherwise give that root a new identity on its way out.
        style_engine.set_tree_scope_root(tree_scope_of(shadow_root), shadow_root.style_node_id());
    }
    // A shadow root built from the document's styles rather than its own decides with the author
    // origin from there, which is otherwise bounded by the scope it is attached to.
    if (shadow_root.uses_document_style_sheets())
        style_engine.set_tree_scope_uses_document_sheets(tree_scope_of(shadow_root));
    // The host link is established every time rather than only when the identity is minted, because
    // the two can be asked for in either order: a root whose identity was taken while its host had
    // none would otherwise stay unlinked once the host arrived.
    if (auto host = shadow_root.host(); host && host->style_node_id() != no_style_node)
        style_engine.set_shadow_root(host->style_node_id(), shadow_root.style_node_id());
    return shadow_root.style_node_id();
}

// The style scope a node belongs to.
//
// The document is scope zero. A shadow root's scope is numbered once and kept for as long as the
// root belongs to this document, which is what makes an attach and its later detach name the same
// thing - the root's place in the style tree is given up and retaken every time it disconnects, and
// a scope that moved with it would leave every sheet adopted into it attached forever.
static TreeScopeID tree_scope_of(DOM::Node& document_or_shadow_root)
{
    auto* shadow_root = as_if<DOM::ShadowRoot>(document_or_shadow_root);
    if (!shadow_root)
        return document_tree_scope;
    if (shadow_root->style_engine_tree_scope() == document_tree_scope)
        shadow_root->set_style_engine_tree_scope(shadow_root->document().style_computer().allocate_tree_scope());
    return shadow_root->style_engine_tree_scope();
}

template<typename Callback>
static void for_each_shadow_including_inclusive_descendant_with_scope(DOM::Node& node, TreeScopeID tree_scope, Callback& callback)
{
    callback(node, tree_scope);

    if (auto* element = as_if<DOM::Element>(node); element && element->shadow_root()) {
        auto& shadow_root = *element->shadow_root();
        auto shadow_scope = tree_scope_of(shadow_root);
        for_each_shadow_including_inclusive_descendant_with_scope(shadow_root, shadow_scope, callback);
    }

    for (auto* child = node.first_child(); child; child = child->next_sibling())
        for_each_shadow_including_inclusive_descendant_with_scope(*child, tree_scope, callback);
}

TreeScopeID style_engine_tree_scope_for(DOM::Node& document_or_shadow_root)
{
    return tree_scope_of(document_or_shadow_root);
}

// The style-tree parent of an element: its parent element, or the shadow root it is a top-level
// child of.
static StyleNodeID style_tree_parent_of(DOM::Element& element, StyleEngine& style_engine)
{
    if (auto parent = element.parent_element())
        return parent->style_node_id();
    if (auto* shadow_root = as_if<DOM::ShadowRoot>(element.parent()))
        return identity_of_shadow_root(*shadow_root, style_engine);
    return no_style_node;
}

static StyleEngineFFI::FfiTreeRelations relations_of(DOM::Element& element, StyleEngine& style_engine, TreeScopeID tree_scope)
{
    auto assigned_slot = no_style_node;
    if (auto slot = element.assigned_slot_internal())
        assigned_slot = slot->style_node_id();

    return StyleEngineFFI::FfiTreeRelations {
        .parent = style_tree_parent_of(element, style_engine).value(),
        .previous_element_sibling = identity_of(element.previous_element_sibling()).value(),
        .next_element_sibling = identity_of(element.next_element_sibling()).value(),
        .tree_scope = tree_scope.value(),
        .assigned_slot = assigned_slot.value(),
        .reserved = 0,
    };
}

static StyleEngineFFI::FfiTreeRelations relations_of(DOM::Element& element, StyleEngine& style_engine)
{
    return relations_of(element, style_engine, tree_scope_of(element.root()));
}

static void record_feature(
    DOM::Element&,
    StyleEngineFFI::FfiFeatureKind,
    StyleAtomID name_atom,
    StyleEngineFFI::FfiFeatureValueKind old_kind,
    StyleAtomID old_atom,
    StyleEngineFFI::FfiFeatureValueKind new_kind,
    StyleAtomID new_atom);

static StyleEngineFFI::FfiTreeRelations detached_relations()
{
    return StyleEngineFFI::FfiTreeRelations {
        .parent = no_style_node.value(),
        .previous_element_sibling = no_style_node.value(),
        .next_element_sibling = no_style_node.value(),
        .tree_scope = 0,
        .assigned_slot = no_style_node.value(),
        .reserved = 0,
    };
}

void record_element_connected(DOM::Element& element)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine)
        return;
    Optional<TreeScopeID> preallocated_tree_scope;
    if (element.style_node_id() == no_style_node) {
        element.set_style_node_id(style_engine->allocate_style_node());
        element.document().style_computer().register_style_node(element.style_node_id(), element);
    } else {
        preallocated_tree_scope = style_engine->consume_preallocated_style_node(element.style_node_id());
        if (!preallocated_tree_scope.has_value()) {
            // Already connected as far as the engine is concerned. Re-recording an insertion would
            // double-link the element into its sibling sequence.
            return;
        }
    }
    // A shadow root that took its identity before its host had one is still waiting to be linked to
    // it. A sheet adopted into a shadow tree names that root, so the root can be identified first,
    // and the link is what lets a `:host` or `::slotted()` rule in that tree reach the host instead
    // of the document.
    if (auto shadow_root = element.shadow_root(); shadow_root && shadow_root->style_node_id() != no_style_node)
        style_engine->set_shadow_root(element.style_node_id(), shadow_root->style_node_id());
    style_engine->record_tree_delta({
        .node = element.style_node_id().value(),
        .old_connected = false,
        .new_connected = true,
        .old_relations = detached_relations(),
        .new_relations = preallocated_tree_scope.has_value()
            ? relations_of(element, *style_engine, *preallocated_tree_scope)
            : relations_of(element, *style_engine),
    });
    style_engine->defer_element_initial_features(element.style_node_id());

    // A slot can take its identity after the nodes assigned to it took theirs - a shadow tree built
    // from markup assigns before the slot connects - and a slottable's assignment is published as
    // the slot's identity, so it was published as no slot at all. Publishing it again from the
    // slot's own arrival is what lets `::slotted()` be answered.
    if (auto* slot = as_if<HTML::HTMLSlotElement>(element)) {
        for (auto const& slottable : slot->assigned_nodes_internal()) {
            if (auto const* assigned = slottable.get_pointer<GC::Ref<DOM::Element>>())
                record_element_assigned_slot_changed(**assigned, nullptr);
        }
    }
}

void publish_pending_element_features(StyleEngine& style_engine, StyleComputer& style_computer)
{
    auto nodes = style_engine.take_deferred_element_initial_features().values();
    // Feature postings are ordered by node identity. Publish arrivals in that order so growing
    // their indexes appends members instead of repeatedly searching and splitting posting chunks.
    quick_sort(nodes);
    for (auto node : nodes) {
        if (auto element = style_computer.element_for_style_node(node))
            record_element_initial_features(*element);
    }
}

void prepare_style_nodes_for_subtree(DOM::Node& root)
{
    if (!root.parent() || !root.parent()->is_connected())
        return;
    auto& style_computer = root.document().style_computer();
    auto& style_engine = style_computer.style_engine();
    Vector<GC::Ptr<DOM::Element>> elements;
    Vector<TreeScopeID> element_tree_scopes;
    Vector<GC::Ptr<DOM::ShadowRoot>> shadow_roots;
    Vector<TreeScopeID> shadow_root_tree_scopes;
    auto collect = [&](DOM::Node& node, TreeScopeID tree_scope) {
        if (auto* element = as_if<DOM::Element>(node); element && element->style_node_id() == no_style_node) {
            elements.append(element);
            element_tree_scopes.append(tree_scope);
        } else if (auto* shadow_root = as_if<DOM::ShadowRoot>(node); shadow_root && shadow_root->style_node_id() == no_style_node) {
            shadow_roots.append(shadow_root);
            shadow_root_tree_scopes.append(tree_scope);
        }
    };
    auto root_tree_scope = tree_scope_of(root.root());
    for_each_shadow_including_inclusive_descendant_with_scope(root, root_tree_scope, collect);
    Vector<StyleNodeID> identities;
    identities.resize(elements.size() + shadow_roots.size());
    style_engine.allocate_style_nodes(identities.span());
    if (!identities.is_empty())
        style_computer.ensure_style_node_slot(identities.last());
    for (size_t index = 0; index < elements.size(); ++index) {
        auto& element = *elements[index];
        element.set_style_node_id(identities[index]);
        style_computer.register_style_node(identities[index], element);
        style_engine.mark_style_node_preallocated(element.style_node_id(), element_tree_scopes[index]);
    }
    for (size_t index = 0; index < shadow_roots.size(); ++index) {
        auto& shadow_root = *shadow_roots[index];
        auto identity = identities[elements.size() + index];
        shadow_root.set_style_node_id(identity);
        style_engine.set_tree_scope_root(shadow_root_tree_scopes[index], identity);
    }
}

enum class InvalidateLanguageCache {
    No,
    Yes,
};

// Publish every selector-visible fact intrinsic to one element. Connected elements and isolated
// selector queries differ only in how an initial local feature delta describes its old side.
template<typename PublishFeature, typename PublishEmptiness>
static void publish_element_selector_features(StyleEngine& style_engine, DOM::Element& element, StyleNodeID node, PublishFeature publish_feature, PublishEmptiness publish_emptiness, InvalidateLanguageCache invalidate_language_cache)
{
    // Slot identity and namespace never change during an element's lifetime.
    auto is_slot = is<HTML::HTMLSlotElement>(element);
    StyleAtomID namespace_atom;
    if (auto const& namespace_uri = element.namespace_uri(); namespace_uri.has_value() && !namespace_uri->is_empty())
        namespace_atom = style_engine.intern_atom(*namespace_uri);

    publish_feature(StyleEngineFFI::FfiFeatureKind::TagName, StyleAtomID {}, StyleEngineFFI::FfiFeatureValueKind::Atom, style_engine.intern_atom(element.local_name()));
    if (auto folded_name = element.local_name().to_ascii_lowercase(); folded_name != element.local_name())
        publish_feature(StyleEngineFFI::FfiFeatureKind::FoldedTagName, StyleAtomID {}, StyleEngineFFI::FfiFeatureValueKind::Atom, style_engine.intern_atom(folded_name));
    if (auto const& id = element.id(); id.has_value())
        publish_feature(StyleEngineFFI::FfiFeatureKind::Id, StyleAtomID {}, StyleEngineFFI::FfiFeatureValueKind::Atom, intern_id_or_class_atom(style_engine, element, *id));
    for (auto const& class_name : element.class_names())
        publish_feature(StyleEngineFFI::FfiFeatureKind::Class, intern_id_or_class_atom(style_engine, element, class_name), StyleEngineFFI::FfiFeatureValueKind::Present, StyleAtomID {});
    element.for_each_attribute([&](DOM::QualifiedName const& name, Utf16View value) {
        auto name_atom = style_engine.intern_attribute_name(name.local_name(), name.namespace_());
        publish_feature(StyleEngineFFI::FfiFeatureKind::Attribute, name_atom, StyleEngineFFI::FfiFeatureValueKind::Atom, style_engine.intern_attribute_value(name_atom, value));
    });

    bool has_nonempty_text_child = false;
    for (GC::Ptr<DOM::Node const> child = element.first_child(); child; child = child->next_sibling()) {
        if (GC::Ptr<DOM::Text const> text = as_if<DOM::Text>(*child); text && !text->data().is_empty()) {
            has_nonempty_text_child = true;
            break;
        }
    }
    publish_emptiness(has_nonempty_text_child);

    auto states = SelectorMatching::element_states(element);
    for (size_t index = 0; index < to_underlying(PseudoClass::__Count); ++index) {
        auto pseudo_class = static_cast<PseudoClass>(index);
        if (!states.get(pseudo_class))
            continue;
        auto fact = state_fact_for(pseudo_class);
        if (fact.has_value())
            style_engine.record_state_delta({ .node = node.value(), .fact = *fact, .new_value = true });
    }

    auto const language = element.lang_view();
    auto language_atom = language.has_value() ? style_engine.intern_language_atom(*language) : StyleAtomID {};
    auto const directionality = element.directionality() == DOM::Element::Directionality::Rtl ? "rtl"_utf16_fly_string : "ltr"_utf16_fly_string;
    auto directionality_atom = style_engine.intern_atom(directionality);
    if (invalidate_language_cache == InvalidateLanguageCache::Yes)
        element.invalidate_lang_value();

    GC::Ptr<HTML::HTMLHeadingElement const> heading = as_if<HTML::HTMLHeadingElement>(element);
    auto heading_level = static_cast<u8>(min(heading ? heading->heading_level() : 0, 255u));

    Vector<StyleAtomID> custom_states;
    if (auto states = element.custom_state_set()) {
        for (auto const& state : states->states())
            custom_states.append(style_engine.intern_atom(state));
    }
    style_engine.record_element_arrival({
                                            .node = node.value(),
                                            .namespace_atom = namespace_atom.value(),
                                            .language_atom = language_atom.value(),
                                            .directionality_atom = directionality_atom.value(),
                                            .custom_state_offset = 0,
                                            .custom_state_count = 0,
                                            .heading_level = heading_level,
                                            .is_slot = is_slot,
                                            .reserved = 0,
                                            .adjustment_facts = element_style_adjustment_facts(element),
                                        },
        custom_states);
}

// Whether the element's cascade may include presentational hints. The hints themselves are
// collected during the C++ computation, and a table cell's read the table's computed style, so
// this decides from the element kind and its attributes alone, conservatively.
// Hints mapped from another element's attributes, which move without the element's own moving.
static bool element_may_have_derived_presentational_hints(DOM::Element const& element)
{
    // A table cell's hints also come from its table's attributes, and an image's from the
    // <source> its <picture> selected.
    if (element.namespace_uri() == Namespace::HTML && first_is_one_of(element.local_name(), HTML::TagNames::td, HTML::TagNames::th, HTML::TagNames::img))
        return true;
    // A body's link, vlink and alink attributes are presentational hints on every link, by the
    // link's :link, :visited and :active state.
    if ((element.matches_link_pseudo_class() || element.matches_visited_pseudo_class())
        && (element.document().normal_link_color().has_value() || element.document().visited_link_color().has_value() || element.document().active_link_color().has_value()))
        return true;
    return false;
}

static bool element_may_have_presentational_hints(DOM::Element const& element)
{
    if (element_may_have_derived_presentational_hints(element))
        return true;
    if (element.publishes_presentational_hints_on_arrival())
        return false;
    // The cascade also reads the width and height attributes of an element that supports them.
    if (element.supports_dimension_attributes()
        && (element.has_attribute(HTML::AttributeNames::width) || element.has_attribute(HTML::AttributeNames::height)))
        return true;
    bool has_presentational_hint = false;
    element.for_each_attribute([&](Utf16FlyString const& name, Utf16View) {
        if (!has_presentational_hint && element.is_presentational_hint(name))
            has_presentational_hint = true;
    });
    return has_presentational_hint;
}

u32 element_box_type_adjustment_facts(DOM::Element const& element)
{
    bool is_html_element = element.namespace_uri() == Namespace::HTML;
    auto local_name = element.local_name();

    bool input_allows_adjustment = false;
    bool input_is_single_line = false;
    if (is<HTML::HTMLInputElement>(element)) {
        auto const& input = static_cast<HTML::HTMLInputElement const&>(element);
        input_allows_adjustment = !first_is_one_of(
            input.type_state(),
            HTML::HTMLInputElement::TypeAttributeState::Hidden,
            HTML::HTMLInputElement::TypeAttributeState::SubmitButton,
            HTML::HTMLInputElement::TypeAttributeState::Button,
            HTML::HTMLInputElement::TypeAttributeState::ResetButton,
            HTML::HTMLInputElement::TypeAttributeState::ImageButton,
            HTML::HTMLInputElement::TypeAttributeState::Checkbox,
            HTML::HTMLInputElement::TypeAttributeState::RadioButton);
        input_is_single_line = input_allows_adjustment && input.is_single_line();
    }

    bool force_position_static = false;
    if (element.namespace_uri() == Namespace::SVG) {
        force_position_static = true;
        if (local_name == "svg"sv) {
            force_position_static = false;
            for (auto ancestor = element.parent_element(); ancestor; ancestor = ancestor->parent_element()) {
                if (ancestor->namespace_uri() == Namespace::SVG && ancestor->local_name() == "foreignObject"sv)
                    break;
                if (ancestor->namespace_uri() == Namespace::SVG && ancestor->local_name() == "svg"sv) {
                    force_position_static = true;
                    break;
                }
            }
        }
    }

    bool force_symbol_display_inline = false;
    if (element.namespace_uri() == Namespace::SVG && local_name == "symbol"sv) {
        if (auto* shadow_root = as_if<DOM::ShadowRoot>(element.parent())) {
            if (auto* host = shadow_root->host())
                force_symbol_display_inline = host->namespace_uri() == Namespace::SVG && host->local_name() == "use"sv;
        }
    }

    u32 facts = 0;
    auto set = [&](bool condition, ElementStyleAdjustmentFact fact) {
        if (condition)
            facts |= fact;
    };
    set(is<HTML::HTMLBRElement>(element), ElementStyleAdjustmentFact::IsBr);
    set(is_html_element && local_name == HTML::TagNames::wbr, ElementStyleAdjustmentFact::IsWbr);
    set(input_allows_adjustment || (is_html_element && first_is_one_of(local_name, HTML::TagNames::textarea, HTML::TagNames::audio, HTML::TagNames::video, HTML::TagNames::canvas, HTML::TagNames::object, HTML::TagNames::iframe, HTML::TagNames::progress, HTML::TagNames::embed, HTML::TagNames::frame, HTML::TagNames::meter, HTML::TagNames::frameset, HTML::TagNames::img)), ElementStyleAdjustmentFact::DisallowDisplayContents);
    set(input_allows_adjustment || (is_html_element && first_is_one_of(local_name, HTML::TagNames::textarea, HTML::TagNames::audio, HTML::TagNames::video, HTML::TagNames::select)), ElementStyleAdjustmentFact::RewriteInlineFlow);
    set(is_html_element && local_name == HTML::TagNames::button, ElementStyleAdjustmentFact::IsButton);
    set(is_html_element && local_name == HTML::TagNames::select, ElementStyleAdjustmentFact::ForceLineHeightNormal);
    set(input_is_single_line, ElementStyleAdjustmentFact::CheckInputLineHeight);
    set(is_html_element && local_name == HTML::TagNames::audio && !element.has_attribute(HTML::AttributeNames::controls), ElementStyleAdjustmentFact::HideAudioWithoutControls);
    set(is_html_element && local_name == HTML::TagNames::table, ElementStyleAdjustmentFact::IsTable);
    set(force_position_static, ElementStyleAdjustmentFact::ForcePositionStatic);
    set(force_symbol_display_inline, ElementStyleAdjustmentFact::ForceSymbolDisplayInline);
    set(element.namespace_uri() == Namespace::MathML, ElementStyleAdjustmentFact::IsMathML);
    set(local_name.equals_ignoring_ascii_case("mtable"sv), ElementStyleAdjustmentFact::IsMathMLMtable);
    set(local_name.equals_ignoring_ascii_case("mtr"sv), ElementStyleAdjustmentFact::IsMathMLMtr);
    set(local_name.equals_ignoring_ascii_case("mtd"sv), ElementStyleAdjustmentFact::IsMathMLMtd);
    set(is_html_element && local_name.equals_ignoring_ascii_case(HTML::TagNames::th), ElementStyleAdjustmentFact::IsTh);
    set(element.is_document_element(), ElementStyleAdjustmentFact::IsDocumentElement);
    return facts;
}

u32 element_style_adjustment_facts(DOM::Element const& element)
{
    auto facts = element_box_type_adjustment_facts(element);
    auto set = [&](bool condition, ElementStyleAdjustmentFact fact) {
        if (condition)
            facts |= fact;
    };
    // Admission facts do not participate in box transformations. In particular, collecting
    // presentational hints can scan every attribute, so only collect them for the engine.
    // An animation the element is associated with composes into its style once it is relevant,
    // which its timeline can make it after the element's arrival.
    set(element.has_relevant_animations() || element.has_associated_animations(), ElementStyleAdjustmentFact::HasAnimations);
    set(element_may_have_presentational_hints(element), ElementStyleAdjustmentFact::HasPresentationalHints);
    set(element.associated_shadow_host_pseudo_element().has_value(), ElementStyleAdjustmentFact::IsShadowHostPseudoElement);
    set(element_may_have_derived_presentational_hints(element), ElementStyleAdjustmentFact::HasDerivedPresentationalHints);
    return facts;
}

void record_element_adjustment_facts(DOM::Element& element)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node)
        return;
    style_engine->set_element_adjustment_facts(element.style_node_id(), element_style_adjustment_facts(element));
}

void publish_required_attribute_value_texts(StyleEngine& style_engine, StyleComputer& style_computer)
{
    style_computer.for_each_style_node([&](DOM::Element& element) {
        element.for_each_attribute([&](DOM::QualifiedName const& name, Utf16View value) {
            auto name_atom = style_engine.intern_attribute_name(name.local_name(), name.namespace_());
            style_engine.backfill_attribute_value_text_if_required(name_atom, value);
        });
    });
}

void configure_isolated_selector_query_engine(StyleEngine& style_engine, DOM::Document& document)
{
    style_engine.set_fold_id_and_class_name_case(document.in_quirks_mode());
    style_engine.set_html_element_namespace(
        document.document_type() == DOM::Document::Type::HTML
            ? style_engine.intern_case_sensitive_text_atom(Namespace::HTML.view())
            : 0);
}

void populate_isolated_selector_query_engine(StyleEngine& style_engine, DOM::ParentNode& root, Function<void(GC::Ref<DOM::Element>, StyleNodeID)> const& publish_identity)
{
    Optional<StyleNodeID> non_element_root_identity;
    if (!is<DOM::Element>(root) && !is<DOM::Document>(root)) {
        non_element_root_identity = style_engine.allocate_style_node();
        style_engine.record_local_feature_delta({
            .node = non_element_root_identity->value(),
            .feature_kind = StyleEngineFFI::FfiFeatureKind::TagName,
            .name_atom = 0,
            .old_kind = StyleEngineFFI::FfiFeatureValueKind::Absent,
            .old_atom = 0,
            .new_kind = StyleEngineFFI::FfiFeatureValueKind::Atom,
            .new_atom = style_engine.intern_atom(Utf16FlyString::from_utf16(u"#document-fragment"sv)).value(),
        });
        style_engine.record_tree_delta({
            .node = non_element_root_identity->value(),
            .old_connected = false,
            .new_connected = true,
            .old_relations = detached_relations(),
            .new_relations = {
                .parent = no_style_node.value(),
                .previous_element_sibling = no_style_node.value(),
                .next_element_sibling = no_style_node.value(),
                .tree_scope = document_tree_scope.value(),
                .assigned_slot = no_style_node.value(),
                .reserved = 0,
            },
        });
    }

    HashMap<GC::Ptr<DOM::Element>, StyleNodeID> identities;
    size_t element_count = 0;
    root.for_each_in_inclusive_subtree_of_type<DOM::Element>([&](DOM::Element&) {
        ++element_count;
        return TraversalDecision::Continue;
    });
    Vector<StyleNodeID> allocated_identities;
    allocated_identities.resize(element_count);
    style_engine.allocate_style_nodes(allocated_identities.span());
    size_t identity_index = 0;
    root.for_each_in_inclusive_subtree_of_type<DOM::Element>([&](DOM::Element& element) {
        auto identity = allocated_identities[identity_index++];
        identities.set(element, identity);
        publish_identity(GC::Ref { element }, identity);
        return TraversalDecision::Continue;
    });

    auto identity_of_element = [&](GC::Ptr<DOM::Element> element) -> StyleNodeID {
        if (!element)
            return no_style_node;
        return identities.get(element).value_or(no_style_node);
    };
    auto record_query_feature = [&](StyleNodeID node, StyleEngineFFI::FfiFeatureKind kind, StyleAtomID name_atom, StyleEngineFFI::FfiFeatureValueKind value_kind, StyleAtomID value_atom) {
        style_engine.record_local_feature_delta({
            .node = node.value(),
            .feature_kind = kind,
            .name_atom = name_atom.value(),
            .old_kind = StyleEngineFFI::FfiFeatureValueKind::Absent,
            .old_atom = 0,
            .new_kind = value_kind,
            .new_atom = value_atom.value(),
        });
    };

    root.for_each_in_inclusive_subtree_of_type<DOM::Element>([&](DOM::Element& element) {
        auto node = identities.get(element).value();
        auto parent = identity_of_element(element.parent_element());
        if (parent == no_style_node && element.parent_node() == &root)
            parent = non_element_root_identity.value_or(no_style_node);
        style_engine.record_tree_delta({
            .node = node.value(),
            .old_connected = false,
            .new_connected = true,
            .old_relations = detached_relations(),
            .new_relations = {
                .parent = parent.value(),
                .previous_element_sibling = identity_of_element(element.previous_element_sibling()).value(),
                .next_element_sibling = identity_of_element(element.next_element_sibling()).value(),
                .tree_scope = document_tree_scope.value(),
                .assigned_slot = no_style_node.value(),
                .reserved = 0,
            },
        });

        publish_element_selector_features(
            style_engine,
            element,
            node,
            [&](auto kind, auto name_atom, auto value_kind, auto value_atom) {
                record_query_feature(node, kind, name_atom, value_kind, value_atom);
            },
            [&](bool has_nonempty_text_child) {
                record_query_feature(node, StyleEngineFFI::FfiFeatureKind::Emptiness, 0, has_nonempty_text_child ? StyleEngineFFI::FfiFeatureValueKind::Absent : StyleEngineFFI::FfiFeatureValueKind::Present, 0);
            },
            InvalidateLanguageCache::No);
        return TraversalDecision::Continue;
    });

    style_engine.flush();
}

// The atom an id or class name is published under. A quirks-mode document matches those selectors
// ASCII case-insensitively, and a selector there is compiled against the lowercase folding of its
// own name, so an element's name has to be folded the same way for the two to name one atom.
static StyleAtomID intern_id_or_class_atom(StyleEngine& style_engine, DOM::Element const& element, Utf16FlyString const& name)
{
    if (element.document().in_quirks_mode())
        return style_engine.intern_atom(name.to_ascii_lowercase());
    return style_engine.intern_atom(name);
}

// An element arrives with facts already true of it, and the engine has heard none of them. They are
// published as ordinary deltas from absent, so the resident fact store is built by exactly the same
// path later mutations take rather than by a second one.
static Optional<StyleEngineFFI::FfiStateFact> state_fact_for(PseudoClass);

static void record_element_initial_features(DOM::Element& element)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node)
        return;

    publish_element_selector_features(
        *style_engine,
        element,
        element.style_node_id(),
        [&](auto kind, auto name_atom, auto value_kind, auto value_atom) {
            style_engine->record_local_feature_delta({
                .node = element.style_node_id().value(),
                .feature_kind = kind,
                .name_atom = name_atom.value(),
                .old_kind = StyleEngineFFI::FfiFeatureValueKind::Absent,
                .old_atom = 0,
                .new_kind = value_kind,
                .new_atom = value_atom.value(),
            });
        },
        [&](bool has_nonempty_text_child) {
            style_engine->record_local_feature_delta({
                .node = element.style_node_id().value(),
                .feature_kind = StyleEngineFFI::FfiFeatureKind::Emptiness,
                .name_atom = 0,
                .old_kind = has_nonempty_text_child ? StyleEngineFFI::FfiFeatureValueKind::Present : StyleEngineFFI::FfiFeatureValueKind::Absent,
                .old_atom = 0,
                .new_kind = has_nonempty_text_child ? StyleEngineFFI::FfiFeatureValueKind::Absent : StyleEngineFFI::FfiFeatureValueKind::Present,
                .new_atom = 0,
            });
        },
        InvalidateLanguageCache::Yes);

    if (!element.part_names().is_empty())
        record_element_parts_changed(element);
    if (auto const inline_style = element.inline_style(); inline_style && (!inline_style->properties().is_empty() || !inline_style->custom_properties().is_empty()))
        record_element_inline_style_properties(element);
    if (element.publishes_presentational_hints_on_arrival() && !element_may_have_derived_presentational_hints(element))
        StyleComputer::collect_presentational_hint_properties({ element });
}

void record_element_moved(DOM::Element& element, DOM::Node* old_parent, DOM::Element* old_previous_sibling, DOM::Element* old_next_sibling)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node)
        return;

    auto relations = relations_of(element, *style_engine);
    auto previous = relations;
    // The parent the element left is a node of the style tree whether or not it is an element: a
    // shadow root is one, and reporting no parent at all would say the element crossed out of the
    // tree rather than moved inside it.
    previous.parent = no_style_node.value();
    if (auto* old_parent_element = as_if<DOM::Element>(old_parent))
        previous.parent = old_parent_element->style_node_id().value();
    else if (auto* old_shadow_root = as_if<DOM::ShadowRoot>(old_parent))
        previous.parent = old_shadow_root->style_node_id().value();
    previous.previous_element_sibling = identity_of(old_previous_sibling).value();
    previous.next_element_sibling = identity_of(old_next_sibling).value();
    if (previous.parent == relations.parent
        && previous.previous_element_sibling == relations.previous_element_sibling
        && previous.next_element_sibling == relations.next_element_sibling) {
        return;
    }

    // A heading's level counts the heading offset its ancestors declare, so moving under a
    // different ancestor can change it without the element itself changing at all.
    record_heading_levels_in_subtree(element);

    if (previous.parent != relations.parent) {
        // Language and directionality resolve through the parent chain, but are published facts
        // rather than computed values. Republish them for the moved subtree from its new place.
        Invalidation::invalidate_style_after_language_change(element);

        // Whether an SVG element's position must become static depends on the SVG and
        // foreignObject elements above it. A preserved move changes that chain without giving
        // the moved subtree another arrival notification.
        element.for_each_shadow_including_inclusive_descendant([&](auto& node) {
            if (auto* descendant = as_if<DOM::Element>(node); descendant && descendant->namespace_uri() == Namespace::SVG && descendant->style_node_id() != no_style_node) {
                style_engine->set_element_adjustment_facts(descendant->style_node_id(), element_style_adjustment_facts(*descendant));
                style_engine->record_element_style_input_change(descendant->style_node_id(), StyleEngine::RecomputeStyle);
            }
            return TraversalDecision::Continue;
        });

        // Moving to a different parent changes the inherited input even if the moved element
        // matches exactly the same rules. Recomputing its style lets ordinary inherited-style
        // propagation carry any change through its light and shadow subtrees.
        style_engine->record_element_style_input_change(element.style_node_id(), StyleEngine::RecomputeStyle);
    }

    // The relinking path rather than the neighbour one. A move rewrites the DOM's own links without
    // going through insertion or removal, so nothing has told the engine's relation columns that
    // anything happened: they are the engine's copy of the child sequence, and only a delta splices
    // them. Publishing this as a neighbour change leaves the columns holding the order the element
    // left, which is the order every positional question is then answered in.
    style_engine->record_tree_delta({
        .node = element.style_node_id().value(),
        .old_connected = true,
        .new_connected = true,
        .old_relations = previous,
        .new_relations = relations,
    });

    // A move across parents is a departure from one child list and an arrival in another, so both
    // parents' emptiness can have moved even though no node was created or destroyed.
    if (old_parent != element.parent()) {
        if (auto* old_parent_element = as_if<DOM::Element>(old_parent))
            record_element_emptiness_changed(*old_parent_element, element, true, false);
        if (auto* new_parent = as_if<DOM::Element>(element.parent()))
            record_element_emptiness_changed(*new_parent, element, false, true);
    }
}

void record_element_assigned_slot_changed(DOM::Element& element, DOM::Element* old_slot)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node)
        return;

    auto relations = relations_of(element, *style_engine);
    auto previous = relations;
    previous.assigned_slot = identity_of(old_slot).value();
    if (previous.assigned_slot == relations.assigned_slot)
        return;

    // The relinking path rather than the neighbour one: the engine holds the slot a slottable is
    // assigned to, and only relinking updates it. Every other relation is identical on both sides,
    // so unlinking and linking again leaves them where they were.
    style_engine->record_tree_delta({
        .node = element.style_node_id().value(),
        .old_connected = true,
        .new_connected = true,
        .old_relations = previous,
        .new_relations = relations,
    });
}

static void record_element_disconnecting(DOM::Element& element, TreeScopeID tree_scope)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine)
        return;
    auto node = element.style_node_id();
    if (node == no_style_node)
        return;

    // Removing a slot can leave its slottables with no replacement. Publish their departure while
    // the slot still has an identity; assignment runs after the disconnect walk and cannot name the
    // old slot by then. If another slot takes over, the ordinary assignment path publishes the
    // subsequent arrival.
    if (auto* slot = as_if<HTML::HTMLSlotElement>(element)) {
        for (auto const& slottable : slot->assigned_nodes_internal()) {
            auto const* assigned_element = slottable.get_pointer<GC::Ref<DOM::Element>>();
            if (!assigned_element || (*assigned_element)->style_node_id() == no_style_node)
                continue;
            auto old_relations = relations_of(**assigned_element, *style_engine);
            auto new_relations = old_relations;
            new_relations.assigned_slot = no_style_node.value();
            style_engine->record_tree_delta({
                .node = (*assigned_element)->style_node_id().value(),
                .old_connected = true,
                .new_connected = true,
                .old_relations = old_relations,
                .new_relations = new_relations,
            });
        }
    }

    style_engine->cancel_deferred_element_initial_features(node);

    style_engine->record_tree_delta({
        .node = node.value(),
        .old_connected = true,
        .new_connected = false,
        .old_relations = relations_of(element, *style_engine, tree_scope),
        .new_relations = detached_relations(),
    });

    // Dropping the identity is what makes a second removal a no-op rather than a double retirement.
    element.document().style_computer().unregister_style_node(node);
    element.set_style_node_id(no_style_node);
}

// The animation names an element's computed style references.
//
// Not an input: the element was recomputed by whatever moved its `animation-name`. This is the index
// that lets a `@keyframes` rule find the elements running the animation it describes, which nothing
// about selector matching can say.
void record_element_animation_names(DOM::Element& element, ReadonlySpan<Utf16FlyString> names)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node)
        return;

    Vector<StyleAtomID> atoms;
    atoms.ensure_capacity(names.size());
    for (auto const& name : names)
        atoms.unchecked_append(style_engine->intern_atom(name));
    style_engine->set_element_animation_names(element.style_node_id(), atoms);
}

// The custom properties an element declares or references.
//
// Also an index rather than an input, and for the same reason as the animation names: what it answers
// is which elements an `@property` registration reaches, and nothing about selector matching or the
// element's own recomputation can say that.
void record_element_custom_property_names(DOM::Element& element, CustomPropertyData const* data, ReadonlySpan<RefPtr<CustomPropertyData const>> pseudo_element_data, ReadonlySpan<Utf16FlyString> references, bool uses_unnamed, bool uses_custom_functions)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node)
        return;

    Vector<StyleAtomID> atoms;
    auto append_declared_names = [&](CustomPropertyData const* custom_property_data) {
        if (!custom_property_data)
            return;
        auto names = custom_property_data->declared_name_atoms(bit_cast<FlatPtr>(&element.document()), style_engine->atom_generation(), [&](Utf16FlyString const& name) { return style_engine->intern_atom(name); });
        atoms.append(names.data(), names.size());
    };
    append_declared_names(data);
    for (auto const& pseudo_data : pseudo_element_data)
        append_declared_names(pseudo_data.ptr());
    for (auto const& name : references)
        atoms.append(style_engine->intern_atom(name));
    quick_sort(atoms);
    size_t unique_count = 0;
    for (size_t index = 0; index < atoms.size(); ++index) {
        if (index == 0 || atoms[index] != atoms[unique_count - 1])
            atoms[unique_count++] = atoms[index];
    }
    atoms.shrink(unique_count);
    style_engine->set_element_custom_property_names(element.style_node_id(), atoms, uses_unnamed, uses_custom_functions);
}

void record_element_custom_property_names(DOM::Element& element, ReadonlySpan<Utf16FlyString> names, bool uses_unnamed, bool uses_custom_functions)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node)
        return;

    Vector<StyleAtomID> atoms;
    atoms.ensure_capacity(names.size());
    for (auto const& name : names)
        atoms.unchecked_append(style_engine->intern_atom(name));
    style_engine->set_element_custom_property_names(element.style_node_id(), atoms, uses_unnamed, uses_custom_functions);
}

// An element's heading level, which `:heading()` tests. It follows from what the element is plus
// the heading offset its ancestors declare, so it moves when the element does and when one of those
// attributes changes - not with anything the element itself publishes.
static void record_element_heading_level(DOM::Element& element)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node || has_pending_initial_features(element))
        return;

    auto const* heading = as_if<HTML::HTMLHeadingElement>(element);
    // Attribute invalidation runs before the document tree version is bumped, so the DOM-facing
    // heading_level() cache can still hold the value from before a headingoffset mutation.
    auto level = heading ? heading->computed_heading_level() : 0;
    style_engine->set_element_heading_level(element.style_node_id(), static_cast<u8>(min(level, 255u)));
}

// Republish the heading level of every element under one whose heading offset just moved.
static void record_heading_levels_in_subtree(DOM::Element& element)
{
    element.for_each_shadow_including_inclusive_descendant([](auto& node) {
        if (auto* descendant = as_if<DOM::Element>(node))
            record_element_heading_level(*descendant);
        return TraversalDecision::Continue;
    });
}

void record_element_language_and_directionality(DOM::Element& element)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node || has_pending_initial_features(element))
        return;

    auto const language = element.lang_view();
    style_engine->set_element_language(
        element.style_node_id(),
        language.has_value() ? style_engine->intern_text_atom(*language) : 0,
        language.value_or({}));

    auto const directionality = element.directionality() == DOM::Element::Directionality::Rtl ? "rtl"sv : "ltr"sv;
    style_engine->set_element_directionality(element.style_node_id(), style_engine->intern_text_atom(Utf16View { directionality }));

    // Reading the tag caches it, and this can run while the element is still being built - an XML
    // parser sets attributes after insertion, so the tag read here may not be the one it ends up
    // with. Drop the cache so the next read computes it from the finished element. The published
    // fact is not wrong for having been early: what a `:lang()` rule is routed by is that the tag
    // moved, and the move to its real value is published like any other.
    element.invalidate_lang_value();
}

void record_element_directionality(DOM::Element& element)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node || has_pending_initial_features(element))
        return;

    auto const directionality = element.directionality() == DOM::Element::Directionality::Rtl ? "rtl"sv : "ltr"sv;
    style_engine->set_element_directionality(element.style_node_id(), style_engine->intern_text_atom(Utf16View { directionality }));
}

void record_element_custom_states_changed(DOM::Element& element)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node || has_pending_initial_features(element))
        return;

    Vector<StyleAtomID> atoms;
    if (auto states = element.custom_state_set()) {
        for (auto const& state : states->states())
            atoms.append(style_engine->intern_atom(state));
    }
    style_engine->set_element_custom_states(element.style_node_id(), atoms);
}

// Walk the chain of hosts outwards, carrying the names the element is addressable by at each level.
//
// A part name reaches the scope enclosing the element's own shadow root. Each host along the way
// forwards the names it chose to, under the names it chose, so the walk ends at the first host that
// forwards none of them. It reports both halves of what a `::part()` rule needs: every name the
// element answers to, and how far out the last of them reaches.
//
// The names and the hosts are also reported paired, one entry per name per level. A name reaches the
// tree the host forwarding it stands in and no other, so a rule writing one level's name while its
// outer compound describes another level's host names no element at all - which the union of the
// names and the outermost host on its own cannot express.
static StyleNodeID collect_part_exposure(DOM::Element const& element, Vector<Utf16FlyString>& pair_names, Vector<StyleNodeID>& pair_hosts)
{
    Vector<Utf16FlyString> names;
    for (auto const& part : element.part_names())
        names.append(part);

    StyleNodeID exposing_host;
    for (auto root = element.containing_shadow_root(); root && !names.is_empty();) {
        auto host = root->host();
        if (!host)
            break;
        exposing_host = host->style_node_id();

        for (auto const& name : names) {
            pair_names.append(name);
            pair_hosts.append(exposing_host);
        }

        Vector<Utf16FlyString> forwarded;
        host->for_each_exported_part([&](Utf16View inner, Utf16View outer) {
            auto inner_name = Utf16FlyString::from_utf16(inner);
            if (!names.contains_slow(inner_name))
                return;
            auto outer_name = Utf16FlyString::from_utf16(outer);
            if (!forwarded.contains_slow(outer_name))
                forwarded.append(outer_name);
        });
        names = move(forwarded);
        root = host->containing_shadow_root();
    }
    return exposing_host;
}

void record_element_parts_changed(DOM::Element& element)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node || has_pending_initial_features(element))
        return;

    // A rule naming a forwarded part names the element under the forwarded name, so the names it
    // is exposed under are what it is published as - and the reach alongside them, because a host
    // usually forwards a name under the one it already had, which moves no name at all.
    Vector<Utf16FlyString> pair_names;
    Vector<StyleNodeID> pair_hosts;
    auto const exposing_host = collect_part_exposure(element, pair_names, pair_hosts);

    Vector<StyleAtomID> pair_atoms;
    pair_atoms.ensure_capacity(pair_names.size());
    for (auto const& name : pair_names)
        pair_atoms.unchecked_append(style_engine->intern_atom(name));
    style_engine->set_element_parts(element.style_node_id(), pair_atoms, pair_hosts);

    style_engine->set_element_part_exposure(element.style_node_id(), exposing_host);
}

void record_element_emptiness_changed(DOM::Element& element, DOM::Node const& changing_child, bool counted_before, bool counts_after)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node)
        return;

    // Whether the element is empty either side of the change is decided by the child that moved
    // together with the ones that did not, and the ones that did not are the same both times.
    auto empty_but_for_the_changing_child = SelectorMatching::element_is_empty_ignoring_child(element, changing_child);
    auto was_empty = empty_but_for_the_changing_child && !counted_before;
    auto is_empty = empty_but_for_the_changing_child && !counts_after;
    if (was_empty == is_empty)
        return;

    auto kind_of = [](bool empty) {
        return empty ? StyleEngineFFI::FfiFeatureValueKind::Present : StyleEngineFFI::FfiFeatureValueKind::Absent;
    };
    record_feature(element, StyleEngineFFI::FfiFeatureKind::Emptiness, 0, kind_of(was_empty), 0, kind_of(is_empty), 0);
}

// An element can arrive with a style attribute already written, so this is published on arrival as
// well as when the block is edited.
static void record_element_inline_style_properties(DOM::Element& element)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node || has_pending_initial_features(element))
        return;
    auto const inline_style = element.inline_style();
    style_engine->set_element_inline_style_properties(element.style_node_id(), inline_style ? &inline_style->declaration_block() : nullptr);
}

// The hints an element's attributes map to are published from where the cascade collects them
// rather than from the element's arrival, because one of them cannot be mapped before style: a
// table cell takes its border colour from the table's *computed* border colour, which no element
// has while the document is still being parsed. Asking for them on arrival crashes on the first
// bordered table for that reason. The cascade builds the block anyway, so this costs the call.
//
// An element that publishes its hints on arrival gets its own kind, which tells the engine the hints
// are current and lets it compute the element's style itself.
bool record_element_presentational_hint_properties(DOM::Element& element, ReadonlySpan<StyleProperty> hints)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node || has_pending_initial_features(element))
        return false;
    auto kind = element.publishes_presentational_hints_on_arrival()
        ? StyleEngineFFI::FfiElementDeclarationKind::SvgPresentationAttribute
        : StyleEngineFFI::FfiElementDeclarationKind::PresentationalHint;
    style_engine->set_element_presentational_hint_properties(element.style_node_id(), kind, hints);
    return true;
}

void record_element_declarations_changed(DOM::Element& element, ElementDeclarationKind kind, bool had_declarations, bool has_declarations)
{
    element.document().flush_deferred_style_change_event();
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node || has_pending_initial_features(element))
        return;
    if (!had_declarations && !has_declarations)
        return;

    auto ffi_kind = StyleEngineFFI::FfiElementDeclarationKind::InlineStyle;
    switch (kind) {
    case ElementDeclarationKind::InlineStyle:
        ffi_kind = StyleEngineFFI::FfiElementDeclarationKind::InlineStyle;
        break;
    case ElementDeclarationKind::PresentationalHint:
        ffi_kind = StyleEngineFFI::FfiElementDeclarationKind::PresentationalHint;
        break;
    case ElementDeclarationKind::SvgPresentationAttribute:
        ffi_kind = StyleEngineFFI::FfiElementDeclarationKind::SvgPresentationAttribute;
        break;
    }

    // The block's contents moved even where the CSSOM object did not, so the identity that makes
    // this a change is a version rather than the object's address.
    style_engine->record_element_declaration_delta({
        .node = element.style_node_id().value(),
        .kind = ffi_kind,
        .old_block = had_declarations ? 1u : 0u,
        .new_block = has_declarations ? style_engine->next_declaration_block_version() : 0u,
    });

    // The properties the block covers are published from wherever the block is built. Inline style
    // is built here, because the CSSOM object is the block; a hint is built by the cascade.
    if (kind == ElementDeclarationKind::InlineStyle)
        record_element_inline_style_properties(element);
}

void record_shadow_root_disconnecting(DOM::ShadowRoot& shadow_root)
{
    auto node = shadow_root.style_node_id();
    if (node == no_style_node)
        return;
    if (auto* style_engine = style_engine_for(shadow_root)) {
        auto relations = detached_relations();
        relations.tree_scope = tree_scope_of(shadow_root).value();
        style_engine->record_tree_delta({
            .node = node.value(),
            .old_connected = true,
            .new_connected = false,
            .old_relations = relations,
            .new_relations = detached_relations(),
        });
    }
    shadow_root.set_style_node_id(no_style_node);
}

void record_subtree_disconnecting(DOM::Node& root)
{
    auto root_tree_scope = tree_scope_of(root.root());
    auto disconnect_element = [](DOM::Node& node, TreeScopeID tree_scope) {
        if (auto* element = as_if<DOM::Element>(node))
            record_element_disconnecting(*element, tree_scope);
    };
    for_each_shadow_including_inclusive_descendant_with_scope(root, root_tree_scope, disconnect_element);

    // Only once no element still names a shadow root as its parent can the root give up its own
    // identity.
    auto disconnect_shadow_root = [](DOM::Node& node, TreeScopeID) {
        if (auto* shadow_root = as_if<DOM::ShadowRoot>(node))
            record_shadow_root_disconnecting(*shadow_root);
    };
    for_each_shadow_including_inclusive_descendant_with_scope(root, root_tree_scope, disconnect_shadow_root);
}

void record_shadow_root_connected(DOM::ShadowRoot& shadow_root)
{
    auto* style_engine = style_engine_for(shadow_root);
    if (!style_engine)
        return;
    // The root retakes its place here rather than when whatever next needs it happens to ask. A
    // scope's root is how a sheet attached to that scope is bounded, and an empty shadow tree has no
    // child whose relations would ask for it, so the scope would be left naming a node that has been
    // given up.
    (void)identity_of_shadow_root(shadow_root, *style_engine);
}

// https://html.spec.whatwg.org/multipage/semantics-other.html#case-sensitivity-of-selectors
// A handful of attribute names compare their values ASCII case-insensitively, but only on an HTML
// element in an HTML document. The element half is the namespace each element already publishes;
// this is the document half, and a selector is compiled against it, so it has to be said before any
// rule is.
static void publish_document_kind(DOM::Document& document)
{
    auto& style_engine = document.style_computer().style_engine();
    style_engine.set_html_element_namespace(
        document.document_type() == DOM::Document::Type::HTML
            ? style_engine.intern_case_sensitive_text_atom(Namespace::HTML.view())
            : 0);
}

void record_document_kind(DOM::Document& document)
{
    publish_document_kind(document);
}

// https://drafts.csswg.org/css-cascade-6/#scope-atrule
// An `@scope` with no `<scope-start>` roots at an element rather than at a selector, so nothing the
// compiler reads says where it is. Resolving it here is the same walk the matcher makes, and the
// engine names the answer by identity.
static constexpr StyleNodeID implicit_scope_root_of_the_containing_tree { 0xffffffff };

static StyleNodeID implicit_scope_root_of(StyleSheetState const* owner_style_sheet)
{
    if (!owner_style_sheet)
        return no_style_node;

    // If no <scope-start> is specified, the scoping root is the parent element of the owner node of
    // the stylesheet where the @scope rule is defined.
    if (auto* owner_node = const_cast<StyleSheetState&>(*owner_style_sheet).owner_node()) {
        if (auto parent = owner_node->parent_element())
            return parent->style_node_id();
    }

    // If no such element exists and the containing node tree is a shadow tree, then the scoping root
    // is the shadow host; otherwise it is the root of the containing node tree. Which of those holds
    // is a property of the scope a sheet is being asked in rather than of the sheet - one
    // constructed sheet can be adopted into several - so the engine resolves it while matching.
    return implicit_scope_root_of_the_containing_tree;
}

using CompilationVisitor = Function<bool(RustRule::Type, StyleSheetState const&, Parser::ValueParserFFI::NativeCompilationContext const&, Parser::ValueParserFFI::NativeCompilationResult const&)>;

static void visit_compilation(StyleSheetState const& sheet, u64 rule_identity, DOM::Document const& document, Parser::ValueParserFFI::NativeCompilationPurpose purpose, CompilationVisitor const& visit, Parser::ValueParserFFI::NativeStylePublication const& publication)
{
    MediaEnvironmentSnapshot environment { document };
    Parser::ValueParserFFI::NativeCompilationCallbacks callbacks {
        .context = &visit,
        .import_source = [](void const* source, u64 identity, Parser::ValueParserFFI::NativeStyleSheet const* native_sheet) -> void const* {
            auto const& sheet = *static_cast<StyleSheetState const*>(source);
            auto* import = sheet.import_for_rule(identity);
            VERIFY(import);
            auto* imported = import->loaded_style_sheet();
            VERIFY(imported && imported->native_sheet().handle() == native_sheet);
            return imported;
        },
        .implicit_scope_root = [](void const* source) { return implicit_scope_root_of(static_cast<StyleSheetState const*>(source)).value(); },
        .visit_rule = [](void const* context, void const* source, u64, RustRule::Type rule_type, Parser::ValueParserFFI::NativeCompilationContext const* compilation, Parser::ValueParserFFI::NativeCompilationResult result) { return (*static_cast<CompilationVisitor const*>(context))(rule_type, *static_cast<StyleSheetState const*>(source), *compilation, result); },
    };
    if (purpose == Parser::ValueParserFFI::NativeCompilationPurpose::Rules)
        Parser::ValueParserFFI::rust_style_sheet_compile(sheet.native_sheet().handle(), rule_identity, &sheet, environment.ffi_environment(), &callbacks, &publication);
    else
        Parser::ValueParserFFI::rust_style_sheet_replace_selectors(sheet.native_sheet().handle(), rule_identity, &sheet, environment.ffi_environment(), &callbacks, &publication);
}

struct RuleCompilationContext {
    RuleCompilationContext(StyleEngine& style_engine, SheetID sheet_handle, StyleEngineRuleID before_rule, DOM::Document const& document, StyleComputer& style_computer)
        : style_engine(style_engine)
        , sheet_handle(sheet_handle)
        , before_rule(before_rule)
        , document(document)
        , style_computer(style_computer)
    {
    }

    StyleEngine& style_engine;
    SheetID sheet_handle;
    StyleEngineRuleID before_rule;
    GC::Ref<DOM::Document const> document;
    GC::Ref<StyleComputer> style_computer;
};

static void publish_layer_order_for_sheet(StyleSheetState const& sheet, DOM::Document const& document)
{
    sheet.for_each_owning_style_scope([&](StyleScope& scope) {
        if (&scope.document() == &document)
            scope.publish_cascade_layer_order();
    });
}

static void compile_rules_into(RuleCompilationContext const& context, StyleSheetState const& sheet, u64 rule_identity = 0, Parser::ValueParserFFI::NativeCompilationPurpose purpose = Parser::ValueParserFFI::NativeCompilationPurpose::Rules)
{
    Parser::ValueParserFFI::NativeStylePublication publication {
        .engine = context.style_engine.rust_handle(),
        .sheet = context.sheet_handle.value(),
        .before_rule = context.before_rule.value(),
    };
    CompilationVisitor visit = [&](RustRule::Type rule_type, StyleSheetState const& source, auto const&, auto const& result) {
        if (purpose == Parser::ValueParserFFI::NativeCompilationPurpose::Selectors && result.rule_id != 0)
            context.style_computer->document().bump_style_environment_version();
        if (result.declares_transitions)
            context.style_engine.note_css_transitions_may_observe_style_changes();
        if (rule_type == RustRule::Type::CounterStyle) {
            source.for_each_owning_style_scope([](StyleScope& scope) {
                scope.invalidate_counter_style_cache();
            });
        }
        if (result.rule_id != 0)
            context.style_computer->register_style_engine_sheet_source(source);
        return true;
    };
    visit_compilation(sheet, rule_identity, *context.document, purpose, visit, publication);
    if (purpose == Parser::ValueParserFFI::NativeCompilationPurpose::Rules)
        publish_layer_order_for_sheet(sheet, *context.document);
}

// The sheet a rule's compiled rules belong to. An imported sheet's rules cascade in the importing
// sheet's program, so the handle to compile into is the outermost sheet's. A constructed sheet is
// always its own engine sheet: its ids are held per adopting document, so its raw id member being 0
// does not mean its rules live in another sheet's program.
static StyleSheetState* owning_compiled_sheet(StyleSheetState* sheet)
{
    while (sheet && !sheet->constructed() && sheet->style_engine_sheet_id() == 0) {
        auto* owner = sheet->owner_import();
        if (!owner)
            return nullptr;
        sheet = owner->parent_style_sheet();
    }
    return sheet;
}

static StyleSheetState* owning_compiled_sheet(CSSRule& rule)
{
    return owning_compiled_sheet(rule.parent_style_sheet());
}

// A constructed sheet compiles once per adopting document, so a mutation to it has to be replayed
// into every document engine holding a copy; every other sheet has exactly one owning document.
static void for_each_document_with_engine_copy(StyleSheetState& sheet, auto const& callback)
{
    if (sheet.constructed()) {
        HashTable<DOM::Document*> documents;
        for (auto owner : sheet.owning_documents_or_shadow_roots())
            documents.set(&owner->document());
        for (auto* document : documents)
            callback(*document);
        // A constructed sheet remains bound to its constructor document while it is not adopted.
        // Keep that document's compiled copy synchronized so reattachment can reuse it.
        if (documents.is_empty()) {
            if (auto* document = const_cast<DOM::Document*>(sheet.constructor_document().ptr()))
                callback(*document);
        }
        return;
    }
    if (auto document = sheet.owning_document())
        callback(*document);
}

static void flush_deferred_style_change_events_for_sheet(StyleSheetState& sheet)
{
    for_each_document_with_engine_copy(sheet, [](DOM::Document& document) {
        document.flush_deferred_style_change_event();
    });
}

void flush_deferred_style_change_events_for_rule(CSSRule& rule)
{
    if (auto* sheet = owning_compiled_sheet(rule))
        flush_deferred_style_change_events_for_sheet(*sheet);
}

static bool rule_change_needs_style_environment_bump(RustRule const& rule)
{
    return Parser::ValueParserFFI::rust_rule_change_needs_style_environment_bump(rule.handle());
}
// A rule arrived in one document's engine. Compile it, and everything it brings with it, into the
// position it holds there.
static void record_style_rule_inserted_in(u64 identity, bool changes_environment, StyleSheetState& sheet, DOM::Document& document)
{
    document.flush_deferred_style_change_event();
    auto& style_computer = document.style_computer();
    auto sheet_id = style_computer.style_engine_sheet_id_for(sheet);
    if (sheet_id == 0)
        return;

    if (changes_environment)
        document.bump_style_environment_version();

    RuleCompilationContext context {
        style_computer.style_engine(),
        sheet_id,
        StyleEngineRuleID { StyleEngineFFI::style_engine_native_rule_successor(style_computer.style_engine().rust_handle(), sheet.native_sheet().handle(), identity) },
        document,
        style_computer
    };
    compile_rules_into(context, sheet, identity);
}

// A rule arrived. Compile it, and everything it brings with it, into the position it holds.
void record_style_rule_inserted(CSSRule& rule)
{
    if (auto* source_sheet = rule.parent_style_sheet())
        record_style_rule_inserted(rule.native_rule(), *source_sheet);
}

static void record_style_rule_inserted(u64 identity, bool changes_environment, StyleSheetState& source_sheet)
{
    auto* sheet = owning_compiled_sheet(&source_sheet);
    if (!sheet)
        return;
    for_each_document_with_engine_copy(*sheet, [&](DOM::Document& document) {
        record_style_rule_inserted_in(identity, changes_environment, *sheet, document);
    });
}

void record_style_rule_inserted(RustRule const& rule, StyleSheetState& source_sheet)
{
    record_style_rule_inserted(rule.identity(), rule_change_needs_style_environment_bump(rule), source_sheet);
}

void record_imported_style_sheet_loaded(u64 import_rule_identity, StyleSheetState& source_sheet)
{
    record_style_rule_inserted(import_rule_identity, true, source_sheet);
}

// A rule left. Retire the identities it compiled into, so nothing it decided keeps deciding.
//
// The sheet is named rather than asked for, because removing a rule from its list detaches it: by
// the time this runs the rule no longer knows where it was.
void record_style_rule_removed(CSSRule& rule)
{
    auto* sheet = owning_compiled_sheet(rule);
    if (!sheet)
        return;
    record_style_rule_removed(*sheet, rule.native_rule());
}

void record_style_rule_removed(StyleSheetState& sheet_it_left, RustRule const& rule, StyleSheetState const* detached_import)
{
    for_each_document_with_engine_copy(sheet_it_left, [&](DOM::Document& document) {
        document.flush_deferred_style_change_event();
        auto& style_computer = document.style_computer();
        struct RemovalContext {
            GC::Ref<DOM::Document> document;
            StyleSheetState& sheet;
        } context { document, sheet_it_left };
        StyleEngineFFI::style_engine_remove_native_rule(
            style_computer.style_engine().rust_handle(),
            sheet_it_left.native_sheet().handle(),
            rule.handle(),
            detached_import ? detached_import->native_sheet().handle() : nullptr,
            style_computer.style_engine_sheet_id_for(sheet_it_left).value(),
            &context,
            [](void* opaque, bool changes_environment, bool has_counter_style) {
                auto& context = *static_cast<RemovalContext*>(opaque);
                if (changes_environment)
                    context.document->bump_style_environment_version();
                if (has_counter_style) {
                    context.sheet.for_each_owning_style_scope([&](StyleScope& scope) {
                        if (&scope.node().document() != context.document.ptr())
                            return;
                        scope.invalidate_counter_style_cache();
                    });
                }
            },
            [](void* opaque, u32, bool declares_layer) {
                auto& context = *static_cast<RemovalContext*>(opaque);
                if (declares_layer)
                    publish_layer_order_for_sheet(context.sheet, context.document);
            });
    });
}

// A rule kept its place and its declarations, and changed what it selects.
void record_style_rule_selector_changed(CSSStyleRule& rule)
{
    auto* sheet = owning_compiled_sheet(rule);
    if (!sheet)
        return;

    for_each_document_with_engine_copy(*sheet, [&](DOM::Document& document) {
        document.flush_deferred_style_change_event();
        auto& style_computer = document.style_computer();
        auto sheet_id = style_computer.style_engine_sheet_id_for(*sheet);
        if (sheet_id == 0)
            return;
        RuleCompilationContext context { style_computer.style_engine(), sheet_id, 0, document, style_computer };
        compile_rules_into(context, *sheet, rule.native_rule().identity(), Parser::ValueParserFFI::NativeCompilationPurpose::Selectors);
    });
}

// A rule kept its place and its selector, and changed what it declares.
void record_style_rule_declarations_changed(CSSRule& rule)
{
    if (auto* sheet = rule.parent_style_sheet())
        record_style_rule_declarations_changed(rule.native_rule(), *sheet);
}

void record_style_rule_declarations_changed(RustRule const& rule, StyleSheetState& source_sheet)
{
    auto* sheet = owning_compiled_sheet(&source_sheet);
    if (!sheet)
        return;

    for_each_document_with_engine_copy(*sheet, [&](DOM::Document& document) {
        document.flush_deferred_style_change_event();
        struct ChangeContext {
            GC::Ref<DOM::Document> document;
            bool changes_environment;
        } context { document, rule.type() != RustRule::Type::Keyframe && rule_change_needs_style_environment_bump(rule) };
        auto& style_engine = document.style_computer().style_engine();
        if (StyleEngineFFI::style_engine_native_rule_declarations_changed(
                style_engine.rust_handle(), rule.handle(), &context,
                [](void* opaque, u32) {
                    auto& context = *static_cast<ChangeContext*>(opaque);
                    if (context.changes_environment)
                        context.document->bump_style_environment_version();
                }))
            style_engine.note_css_transitions_may_observe_style_changes();
    });
}

// `replace()` swaps a sheet's whole rule list, so there is nothing of the old one to keep.
void record_stylesheet_rules_replaced(StyleSheetState& sheet)
{
    for_each_document_with_engine_copy(sheet, [&](DOM::Document& document) {
        document.flush_deferred_style_change_event();
        auto& style_computer = document.style_computer();
        auto sheet_id = style_computer.style_engine_sheet_id_for(sheet);
        if (sheet_id == 0)
            return;
        auto& style_engine = style_computer.style_engine();
        style_engine.begin_sheet_rules_replacement(sheet_id);
        RuleCompilationContext context { style_engine, sheet_id, 0, document, style_computer };
        compile_rules_into(context, sheet);
        style_engine.finish_sheet_rules_replacement(sheet_id);
    });
}

void record_stylesheet_attached(StyleSheetState& sheet, DOM::Node& document_or_shadow_root, StyleSheetState* before)
{
    document_or_shadow_root.document().flush_deferred_style_change_event();
    publish_document_kind(document_or_shadow_root.document());
    auto& style_computer = document_or_shadow_root.document().style_computer();
    auto& style_engine = style_computer.style_engine();
    auto sheet_id = style_computer.style_engine_sheet_id_for(sheet);
    auto first_attachment = sheet_id == 0;
    if (first_attachment) {
        // The CSSOM object's identity is what the program keys its wrapper by; the semantic sheet
        // is a separate identity that survives edits to its contents.
        sheet_id = style_engine.add_sheet(
            static_cast<u32>(reinterpret_cast<FlatPtr>(&sheet) >> 3),
            StyleEngineFFI::FfiCascadeOrigin::Author);
        style_computer.set_style_engine_sheet_id_for(sheet, sheet_id);
    }
    // A sheet attached to a shadow root is bounded by the tree it decides in, and what bounds it is
    // that tree's root. Attaching the root to its host numbers the scope but puts nothing in it, so
    // a component that attaches and then adopts - which is the ordinary order - would name a scope
    // the engine cannot place and be answered for with the whole document.
    if (auto* shadow_root = as_if<DOM::ShadowRoot>(document_or_shadow_root))
        (void)identity_of_shadow_root(*shadow_root, style_engine);

    // Naming the successor rather than a position is what lets the engine keep order as tokens: an
    // insertion writes one label and renumbers nothing.
    style_engine.attach_sheet(
        sheet_id,
        tree_scope_of(document_or_shadow_root),
        before ? style_computer.style_engine_sheet_id_for(*before) : 0);

    // A constructed sheet can be configured before anything adopts it, so attachment is its first
    // opportunity to publish the condition state.
    if (sheet.constructed()) {
        sheet.evaluate_media_queries(document_or_shadow_root.document());
        style_engine.set_sheet_conditions_hold(sheet_id, !sheet.disabled() && sheet.native_media_list().matches());
    }

    // Layer ranks belong to the attachment's tree scope. Publishing them while attaching keeps the
    // first transaction for a new shadow tree from matching with the document scope's order. The
    // rule-cache build is too late: it can happen while consuming that transaction's answers.
    if (auto* shadow_root = as_if<DOM::ShadowRoot>(document_or_shadow_root))
        shadow_root->style_scope().publish_cascade_layer_order(&sheet);
    else
        document_or_shadow_root.document().style_scope().publish_cascade_layer_order(&sheet);

    // A sheet arrives with a condition state, and that state is otherwise only published when it
    // moves. A constructed sheet is built disabled or given media before anything adopts it, with no
    // scope to publish either to, so an attachment is where the engine hears them - and its media
    // has never been evaluated against a document before this either. A sheet in a style sheet list
    // is announced by the list, which does this for itself.
    // A sheet's rules belong to the sheet, not to an attachment. Compiling them again when the same
    // sheet is adopted into a second scope, or moved from one to another, would give it two copies
    // of every rule.
    if (!first_attachment)
        return;
    RuleCompilationContext context { style_engine, sheet_id, 0, document_or_shadow_root.document(), style_computer };
    compile_rules_into(context, sheet);
}

// The user-agent and user origins have no style sheet list to attach from, so nothing announces
// them the way an author sheet announces itself. They still carry the rules a great deal of state
// invalidation depends on: `:focus-visible` outlines come from the user-agent sheet, and a content
// blocker's `span:hover` comes from the user one. An engine that owns state invalidation without
// knowing those rules silently stops invalidating for them.
//
// The user-agent sheets are shared between documents, so their StyleEngine identities cannot live on
// the sheet object the way an author sheet's does; they are held here, per document, alongside the
// sheets they name. The user sheet is rebuilt rather than edited when content blockers change, so
// the set is compared by identity and re-attached whole when it differs.
void record_non_author_stylesheets(DOM::Document& document)
{
    auto& style_computer = document.style_computer();
    auto& style_scope = document.style_scope();

    publish_document_kind(document);

    Vector<NonnullRefPtr<StyleSheetState>> sheets;
    Vector<StyleEngineFFI::FfiCascadeOrigin> origins;
    for (auto origin : { CascadeOrigin::UserAgent, CascadeOrigin::User }) {
        style_scope.for_each_stylesheet(origin, [&](StyleSheetState& sheet) {
            sheets.append(sheet);
            origins.append(origin == CascadeOrigin::UserAgent ? StyleEngineFFI::FfiCascadeOrigin::UserAgent : StyleEngineFFI::FfiCascadeOrigin::User);
        });
    }

    auto& recorded = style_computer.non_author_style_sheets();
    auto recorded_sheets_match = [&] {
        if (recorded.size() != sheets.size())
            return false;
        for (size_t index = 0; index < sheets.size(); ++index) {
            if (recorded[index].sheet != sheets[index])
                return false;
        }
        return true;
    };
    if (recorded_sheets_match())
        return;

    document.flush_deferred_style_change_event();
    if (recorded_sheets_match())
        return;

    auto& style_engine = style_computer.style_engine();
    for (auto const& entry : recorded)
        style_engine.detach_sheet(entry.sheet_id, document_tree_scope);
    recorded.clear();

    // These origins cascade before every author sheet, so each is inserted ahead of the first one
    // rather than appended. Inserting each new sheet before that same successor keeps them in the
    // order they were collected.
    SheetID first_author_sheet;
    for (auto const& sheet : document.style_scope().style_sheets()) {
        if (sheet->style_engine_sheet_id() != 0) {
            first_author_sheet = sheet->style_engine_sheet_id();
            break;
        }
    }

    for (size_t index = 0; index < sheets.size(); ++index) {
        auto sheet_id = style_engine.add_sheet(
            static_cast<u32>(reinterpret_cast<FlatPtr>(sheets[index].ptr()) >> 3),
            origins[index]);
        style_engine.attach_sheet(sheet_id, document_tree_scope, first_author_sheet);
        RuleCompilationContext context { style_engine, sheet_id, 0, document, style_computer };
        compile_rules_into(context, *sheets[index]);
        recorded.append({ sheets[index], sheet_id });
    }
}

static StyleSheetState* owning_engine_sheet(StyleSheetState& sheet)
{
    // A constructed sheet is always its own engine sheet; its per-document ids make the raw member 0
    // without its rules living in any other sheet's program.
    if (sheet.constructed())
        return &sheet;
    auto* engine_sheet = &sheet;
    while (engine_sheet->style_engine_sheet_id() == 0) {
        auto* owner = engine_sheet->owner_import();
        if (!owner)
            return nullptr;
        engine_sheet = owner->parent_style_sheet();
        if (!engine_sheet)
            return nullptr;
    }
    return engine_sheet;
}

void record_stylesheet_rule_conditions(StyleSheetState& sheet)
{
    auto* engine_sheet = owning_engine_sheet(sheet);
    if (!engine_sheet)
        return;
    for_each_document_with_engine_copy(*engine_sheet, [&](DOM::Document& document) {
        record_stylesheet_rule_conditions(sheet, document);
    });
}

void record_stylesheet_rule_conditions(StyleSheetState& sheet, DOM::Document& document)
{
    auto* engine_sheet = sheet.owner_import() ? owning_engine_sheet(sheet) : &sheet;
    if (!engine_sheet)
        return;
    document.flush_deferred_style_change_event();
    auto& style_computer = document.style_computer();
    // Imported rules inherit the conditions of every enclosing import. Starting at an imported
    // sheet would lose those gates and could re-enable rules beneath a non-matching import.
    MediaEnvironmentSnapshot environment { document };
    Parser::ValueParserFFI::rust_style_sheet_publish_conditions(
        engine_sheet->native_sheet().handle(), style_computer.style_engine().rust_handle(), environment.ffi_environment());
}

void record_stylesheet_conditions(StyleSheetState& sheet, DOM::Node& document_or_shadow_root, bool conditions_hold)
{
    document_or_shadow_root.document().flush_deferred_style_change_event();
    auto* engine_sheet = owning_engine_sheet(sheet);
    if (!engine_sheet)
        return;
    // An imported sheet gates only its own rules, not the entire enclosing engine sheet.
    if (engine_sheet != &sheet) {
        record_stylesheet_rule_conditions(*engine_sheet, document_or_shadow_root.document());
        return;
    }
    auto& style_computer = document_or_shadow_root.document().style_computer();
    auto sheet_id = style_computer.style_engine_sheet_id_for(*engine_sheet);
    if (sheet_id == 0)
        return;
    style_computer.style_engine().set_sheet_conditions_hold(sheet_id, conditions_hold);
}

void record_stylesheet_detached(StyleSheetState& sheet, DOM::Node& document_or_shadow_root)
{
    document_or_shadow_root.document().flush_deferred_style_change_event();
    auto& style_computer = document_or_shadow_root.document().style_computer();
    auto sheet_id = style_computer.style_engine_sheet_id_for(sheet);
    if (sheet_id == 0)
        return;
    style_computer.style_engine().detach_sheet(sheet_id, tree_scope_of(document_or_shadow_root));
}

// Every boolean pseudo-class the parser can produce has a fact, so the switch is exhaustive over
// them. The pseudo-classes with no entry are the ones StyleEngine models as operators rather than
// facts -- positional, logical, structural, and the parameterized ones -- and those change with
// inputs the engine already routes, not with a state transition published here.
#include <LibWeb/StyleEngineStateFactsGenerated.inc>

bool can_record_element_state_change(DOM::Element& element)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine)
        return false;
    auto node = element.style_node_id();
    if (node == no_style_node || has_pending_initial_features(element))
        return false;
    return true;
}

void record_element_state_changed(DOM::Element& element, PseudoClass pseudo_class, bool new_value)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine)
        return;
    auto node = element.style_node_id();
    if (node == no_style_node || has_pending_initial_features(element))
        return;
    auto fact = state_fact_for(pseudo_class);
    if (!fact.has_value())
        return;

    style_engine->record_state_delta({
        .node = node.value(),
        .fact = *fact,
        .new_value = new_value,
    });
}

static void record_feature(
    DOM::Element& element,
    StyleEngineFFI::FfiFeatureKind kind,
    StyleAtomID name_atom,
    StyleEngineFFI::FfiFeatureValueKind old_kind,
    StyleAtomID old_atom,
    StyleEngineFFI::FfiFeatureValueKind new_kind,
    StyleAtomID new_atom)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine)
        return;
    auto node = element.style_node_id();
    if (node == no_style_node || has_pending_initial_features(element))
        return;

    style_engine->record_local_feature_delta(
        {
            .node = node.value(),
            .feature_kind = kind,
            .name_atom = name_atom.value(),
            .old_kind = old_kind,
            .old_atom = old_atom.value(),
            .new_kind = new_kind,
            .new_atom = new_atom.value(),
        });
}

void record_element_id_changed(DOM::Element& element, Optional<Utf16FlyString> const& old_value, Optional<Utf16FlyString> const& new_value)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node)
        return;

    auto atom_of = [&](Optional<Utf16FlyString> const& value) -> StyleAtomID {
        return value.has_value() ? intern_id_or_class_atom(*style_engine, element, *value) : 0;
    };
    auto kind_of = [](Optional<Utf16FlyString> const& value) {
        return value.has_value() ? StyleEngineFFI::FfiFeatureValueKind::Atom : StyleEngineFFI::FfiFeatureValueKind::Absent;
    };

    record_feature(element, StyleEngineFFI::FfiFeatureKind::Id, 0, kind_of(old_value), atom_of(old_value), kind_of(new_value), atom_of(new_value));
}

void record_element_class_list_changed(DOM::Element& element, Vector<Utf16FlyString> const& old_classes, Vector<Utf16FlyString> const& new_classes)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node)
        return;

    // One class delta per class that actually gained or lost membership. A class present on both
    // sides is not a change, and journalling it would be exactly the amplification the engine
    // exists to avoid.
    // Two class names that differ only in case are one class where the document folds them, so
    // membership is decided between the folded names rather than the written ones.
    auto folded = [&](Utf16FlyString const& name) {
        return element.document().in_quirks_mode() ? name.to_ascii_lowercase() : name;
    };
    auto contains_folded = [&](Vector<Utf16FlyString> const& names, Utf16FlyString const& folded_name) {
        return any_of(names, [&](auto const& name) { return folded(name) == folded_name; });
    };

    auto record_membership = [&](Utf16FlyString const& folded_name, bool was_present, bool is_present) {
        if (was_present == is_present)
            return;
        auto atom = style_engine->intern_atom(folded_name);
        record_feature(
            element,
            StyleEngineFFI::FfiFeatureKind::Class,
            atom,
            was_present ? StyleEngineFFI::FfiFeatureValueKind::Present : StyleEngineFFI::FfiFeatureValueKind::Absent,
            0,
            is_present ? StyleEngineFFI::FfiFeatureValueKind::Present : StyleEngineFFI::FfiFeatureValueKind::Absent,
            0);
    };

    for (auto const& name : old_classes)
        record_membership(folded(name), true, contains_folded(new_classes, folded(name)));
    for (auto const& name : new_classes) {
        if (!contains_folded(old_classes, folded(name)))
            record_membership(folded(name), false, true);
    }
}

void record_element_attribute_changed(DOM::Element& element, Utf16FlyString const& name, Optional<Utf16FlyString> const& namespace_uri, Optional<Utf16String> const& old_value, Optional<Utf16String> const& new_value)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node)
        return;
    if (!old_value.has_value() && !new_value.has_value())
        return;

    // These two are what a heading level counts, and they answer for every heading beneath them.
    if (name == HTML::AttributeNames::headingoffset || name == HTML::AttributeNames::headingreset)
        record_heading_levels_in_subtree(element);

    // Attribute presence decides whether the element cascades presentational hints. Changing
    // only a value cannot move that fact, except for an input's type, which also decides its
    // box adjustments and whether it supports dimension attributes.
    if (old_value.has_value() != new_value.has_value() || name == HTML::AttributeNames::type)
        record_element_adjustment_facts(element);

    // Both values cross as atoms. Their text is recorded once per distinct value only when a
    // compiled selector for this attribute uses an operator that cannot compare atom identities.
    // This lets the match evaluator reconstruct either side of such a transaction without asking
    // the DOM, and two different values cannot cancel in the journal merely because both are
    // present.
    // The same name an arriving attribute publishes, with the same other forms noted alongside it.
    // See `StyleEngine::intern_attribute_name`.
    auto atom = style_engine->intern_attribute_name(name, namespace_uri);
    auto kind_of = [](Optional<Utf16String> const& value) {
        return value.has_value() ? StyleEngineFFI::FfiFeatureValueKind::Atom : StyleEngineFFI::FfiFeatureValueKind::Absent;
    };
    auto atom_of = [&](Optional<Utf16String> const& value) {
        return value.has_value() ? style_engine->intern_attribute_value(atom, *value) : 0;
    };
    auto old_kind = kind_of(old_value);
    auto old_atom = atom_of(old_value);
    auto new_kind = kind_of(new_value);
    auto new_atom = atom_of(new_value);
    record_feature(
        element,
        StyleEngineFFI::FfiFeatureKind::Attribute,
        atom,
        old_kind,
        old_atom,
        new_kind,
        new_atom);
}

}
