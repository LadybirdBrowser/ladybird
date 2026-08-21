/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/CSSConditionRule.h>
#include <LibWeb/CSS/CSSContainerRule.h>
#include <LibWeb/CSS/CSSCounterStyleRule.h>
#include <LibWeb/CSS/CSSFontFeatureValuesRule.h>
#include <LibWeb/CSS/CSSFunctionRule.h>
#include <LibWeb/CSS/CSSGroupingRule.h>
#include <LibWeb/CSS/CSSImportRule.h>
#include <LibWeb/CSS/CSSKeyframeRule.h>
#include <LibWeb/CSS/CSSKeyframesRule.h>
#include <LibWeb/CSS/CSSLayerBlockRule.h>
#include <LibWeb/CSS/CSSLayerStatementRule.h>
#include <LibWeb/CSS/CSSMediaRule.h>
#include <LibWeb/CSS/CSSNestedDeclarations.h>
#include <LibWeb/CSS/CSSPropertyRule.h>
#include <LibWeb/CSS/CSSRuleList.h>
#include <LibWeb/CSS/CSSScopeRule.h>
#include <LibWeb/CSS/CSSStyleRule.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/CSSSupportsRule.h>
#include <LibWeb/CSS/Invalidation/LanguageInvalidator.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/SelectorMatching.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/CSS/StyleSheetList.h>
#include <LibWeb/CSS/StyleValues/ColorFunctionStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/CustomElements/CustomStateSet.h>
#include <LibWeb/HTML/HTMLHeadingElement.h>
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
    for (auto node : style_engine.take_deferred_element_initial_features()) {
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
        namespace_atom = style_engine.intern_case_sensitive_text_atom(namespace_uri->view());

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
        auto fact = state_fact_for(pseudo_class);
        if (fact.has_value() && states.get(pseudo_class))
            style_engine.record_state_delta({ .node = node.value(), .fact = *fact, .new_value = true });
    }

    auto const language = element.lang_view();
    auto language_atom = language.has_value() ? style_engine.intern_language_atom(*language) : StyleAtomID {};
    auto const directionality = element.directionality() == DOM::Element::Directionality::Rtl ? "rtl"sv : "ltr"sv;
    auto directionality_atom = style_engine.intern_text_atom(Utf16View { directionality });
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
                                        },
        custom_states);
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
void record_element_custom_property_names(DOM::Element& element, CustomPropertyData const* data, bool uses_unnamed, bool uses_custom_functions)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node)
        return;

    auto atoms = data
        ? data->declared_name_atoms(bit_cast<FlatPtr>(&element.document()), style_engine->atom_generation(), [&](Utf16FlyString const& name) { return style_engine->intern_atom(name); })
        : ReadonlySpan<StyleAtomID> {};
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

// The context-free computed form of a specified value, where the value has one.
//
// Numeric rgb() values need no element, inherited style, font, viewport, or other dynamic input to
// compute. Publishing their specified syntax would make "#14181c" and "rgb(20, 24, 28)" distinct
// even though every cascade computes them to the same value.
static ValueComparingNonnullRefPtr<StyleValue const> canonical_specified_value(StyleValue const& value)
{
    if (!value.is_color_function())
        return value;
    auto const& color = as<ColorFunctionStyleValue>(value);
    if (color.origin_color()
        || color.color_type() != ColorStyleValue::ColorType::RGB
        || any_of(color.channels(), [](auto const& channel) { return !channel->is_number(); })
        || (color.alpha() && !color.alpha()->is_number()))
        return value;
    return color.computed_value_form();
}

// Which properties an element's own declarations cover is what the cascade compares them by: an
// element-attached declaration beats every rule in its context, so it decides those properties and
// no others.
static StyleEngineFFI::FfiCascadeOperator cascade_operator_for(StyleValue const& value)
{
    if (!value.is_keyword())
        return StyleEngineFFI::FfiCascadeOperator::Declared;
    switch (value.to_keyword()) {
    case Keyword::Inherit:
        return StyleEngineFFI::FfiCascadeOperator::Inherit;
    case Keyword::Initial:
        return StyleEngineFFI::FfiCascadeOperator::Initial;
    case Keyword::Unset:
        return StyleEngineFFI::FfiCascadeOperator::Unset;
    case Keyword::Revert:
        return StyleEngineFFI::FfiCascadeOperator::Revert;
    case Keyword::RevertLayer:
        return StyleEngineFFI::FfiCascadeOperator::RevertLayer;
    default:
        return StyleEngineFFI::FfiCascadeOperator::Declared;
    }
}

enum class ExpandShorthands {
    No,
    Yes,
};

struct DeclaredPropertyColumns {
    DeclaredPropertyColumns(size_t capacity, bool declarations_are_complete)
        : declarations_are_complete(declarations_are_complete)
    {
        properties.ensure_capacity(capacity);
        important.ensure_capacity(capacity);
        operators.ensure_capacity(capacity);
        values.ensure_capacity(capacity);
        original_values.ensure_capacity(capacity);
        retained_values.ensure_capacity(capacity);
    }

    void append(StyleProperty const& property, ExpandShorthands expand_shorthands)
    {
        auto is_important = property.important == Important::Yes;
        auto cascade_operator = cascade_operator_for(*property.value);
        auto value = canonical_specified_value(*property.value);
        if (property.property_id == PropertyID::All)
            declarations_are_complete = false;
        if (expand_shorthands == ExpandShorthands::Yes && property_is_shorthand(property.property_id)) {
            for (auto longhand : expanded_longhands_for_shorthand(property.property_id)) {
                properties.append(to_underlying(longhand));
                important.append(is_important);
                operators.append(cascade_operator);
                values.append(value->rust_style_value_data());
                original_values.append(property.value->rust_style_value_data());
            }
        } else {
            properties.append(to_underlying(property.property_id));
            important.append(is_important);
            operators.append(cascade_operator);
            values.append(value->rust_style_value_data());
            original_values.append(property.value->rust_style_value_data());
        }
        retained_values.append(move(value));
    }

    Vector<u16> properties;
    Vector<bool> important;
    Vector<StyleEngineFFI::FfiCascadeOperator> operators;
    Vector<void const*> values;
    Vector<void const*> original_values;
    Vector<ValueComparingNonnullRefPtr<StyleValue const>> retained_values;
    bool declarations_are_complete;
};

bool property_defines_a_css_transition(PropertyID property_id)
{
    return property_id == PropertyID::Transition
        || property_id == PropertyID::TransitionBehavior
        || property_id == PropertyID::TransitionDelay
        || property_id == PropertyID::TransitionDuration
        || property_id == PropertyID::TransitionProperty
        || property_id == PropertyID::TransitionTimingFunction;
}

static bool publish_element_declared_properties(DOM::Element& element, StyleEngineFFI::FfiElementDeclarationKind kind, ReadonlySpan<StyleProperty> style_properties, bool declarations_are_complete = true)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node || has_pending_initial_features(element))
        return false;

    DeclaredPropertyColumns columns(style_properties.size(), declarations_are_complete);
    for (auto const& property : style_properties) {
        if (property_defines_a_css_transition(property.property_id))
            style_engine->note_css_transitions_may_observe_style_changes();
        // What a declaration decides is a set of longhands. An attribute maps to whichever property
        // names it, `overflow` included, and the cascade expands that before anything is decided, so
        // a shorthand left whole here would name a property nothing ever wins.
        columns.append(property, ExpandShorthands::Yes);
    }
    style_engine->set_element_declared_properties(element.style_node_id(), kind, columns.properties, columns.important, columns.operators, columns.values, columns.original_values, columns.declarations_are_complete);
    return true;
}

// An element can arrive with a style attribute already written, so this is published on arrival as
// well as when the block is edited.
static void record_element_inline_style_properties(DOM::Element& element)
{
    auto const inline_style = element.inline_style();
    publish_element_declared_properties(
        element,
        StyleEngineFFI::FfiElementDeclarationKind::InlineStyle,
        inline_style ? inline_style->properties().span() : ReadonlySpan<StyleProperty> {},
        !inline_style || inline_style->custom_properties().is_empty());
}

// The hints an element's attributes map to are published from where the cascade collects them
// rather than from the element's arrival, because one of them cannot be mapped before style: a
// table cell takes its border colour from the table's *computed* border colour, which no element
// has while the document is still being parsed. Asking for them on arrival crashes on the first
// bordered table for that reason. The cascade builds the block anyway, so this costs the call.
//
// SVG presentation attributes map through the same hook, so they are published under this kind too.
bool record_element_presentational_hint_properties(DOM::Element& element, ReadonlySpan<StyleProperty> hints)
{
    return publish_element_declared_properties(element, StyleEngineFFI::FfiElementDeclarationKind::PresentationalHint, hints);
}

void record_element_declarations_changed(DOM::Element& element, ElementDeclarationKind kind, bool had_declarations, bool has_declarations)
{
    element.document().flush_deferred_style_change_event();
    // A declaration the element itself sources is named in its style input record by its identity,
    // and this is the write that moves what that identity says without moving the identity.
    element.retire_style_input_record();

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

// Whether a group's condition holds, asked so that the answer is about the condition rather than
// about whether anything has looked at it yet. A freshly parsed media list has evaluated nothing, and
// an unevaluated one reads as not matching - so a rule arriving inside `@media all` would look gated.
static bool condition_holds(CSSRule& rule, DOM::Document const& document)
{
    if (auto* media_rule = as_if<CSSMediaRule>(rule))
        return media_rule->media()->evaluate(document);
    if (auto* supports_rule = as_if<CSSSupportsRule>(rule))
        return supports_rule->condition_matches();
    // An `@import` carries a media query of its own, and it gates everything the imported sheet
    // brings in exactly as an `@media` around those rules would. A sheet imported under
    // `(prefers-color-scheme: dark)` decides nothing in a light document.
    if (auto* import_rule = as_if<CSSImportRule>(rule))
        return import_rule->matches();
    // Only `@media` and `@supports` gate rules for the document as a whole. A container query asks
    // about the element being styled and is answered during matching, so it gates nothing here - and
    // asking it outside that context is not even allowed.
    return true;
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

// Walks a sheet's rules and compiles every style rule's selector list into the program.
//
// The parser has already produced a compiled selector for the matching engine, so this hands over
// that representation rather than re-parsing anything, and no string crosses: a compiled selector
// carries the interned identity of every name it mentions.
//
// `before_rule` is the engine rule the compiled rules go in front of, or 0 to append. Naming a
// successor is what lets rules arrive in the middle of a sheet without renumbering anything.
// Whether a rule declares a cascade layer, and so fixes where that layer sits against the others.
static bool declares_a_layer(CSSRule const& rule)
{
    if (is<CSSLayerStatementRule>(rule) || is<CSSLayerBlockRule>(rule))
        return true;
    auto* import_rule = as_if<CSSImportRule>(rule);
    return import_rule && import_rule->layer_name().has_value();
}

// The `@namespace` declarations in scope for a rule's selectors. A prefix means whatever its sheet
// says it means and an unprefixed type or universal selector means the sheet's default namespace,
// both of which are CSSOM state, so they are resolved here and the compiler looks them up per
// qualified name. Resolving only the subject's would leave `*|x y` unconstrained on `y`'s ancestor.
static StyleEngine::NamespaceScope namespace_scope_of(StyleEngine& style_engine, CSSStyleSheet const* sheet)
{
    StyleEngine::NamespaceScope scope;
    if (!sheet)
        return scope;
    // https://drafts.csswg.org/css-namespaces/#syntax
    // The empty string is a namespace name, and it is the one an element in no namespace has. A
    // declaration of it therefore constrains, where no declaration at all does not - which is why a
    // default declared as the empty string cannot be reported the same way as an absent one.
    if (auto default_namespace = sheet->default_namespace(); default_namespace.has_value()) {
        scope.default_namespace = default_namespace->is_empty()
            ? StyleEngine::no_namespace
            : style_engine.intern_case_sensitive_text_atom(default_namespace->view());
    }
    for (auto const& [prefix, rule] : sheet->namespace_rules()) {
        if (!rule || prefix.is_empty())
            continue;
        scope.prefixes.append(style_engine.intern_case_sensitive_text_atom(prefix.view()));
        scope.uris.append(rule->namespace_uri().is_empty()
                ? 0
                : style_engine.intern_case_sensitive_text_atom(rule->namespace_uri().view()));
    }
    return scope;
}

// The longhand properties a rule declares. Only a property some rule declares can be a candidate
// for a winner change, so the cascade is told which those are rather than reading the declaration
// block back across the boundary.
static void record_rule_declared_properties(StyleEngine& style_engine, StyleEngineRuleID rule_id, CSSRule const& rule)
{
    if (rule_id == 0)
        return;
    CSSStyleProperties const* declaration = nullptr;
    if (auto const* style_rule = as_if<CSSStyleRule>(rule))
        declaration = &style_rule->declaration();
    else if (auto const* nested = as_if<CSSNestedDeclarations>(rule))
        declaration = &nested->declaration();
    if (!declaration)
        return;

    DeclaredPropertyColumns columns(declaration->properties().size(), declaration->custom_properties().is_empty());
    for (auto const& property : declaration->properties()) {
        if (property_defines_a_css_transition(property.property_id))
            style_engine.note_css_transitions_may_observe_style_changes();
        columns.append(property, ExpandShorthands::No);
    }
    style_engine.set_rule_declared_properties(rule_id, columns.properties, columns.important, columns.operators, columns.values, columns.original_values, columns.declarations_are_complete);
}

// Where a compiled rule's identity is written. Author rules carry theirs on the rule object; the
// user-agent and user sheets are process-wide singletons, so their rules cannot, and the document
// that compiled them holds the identity instead.
static void set_compiled_rule_id(CSSRule& rule, StyleEngineRuleID rule_id, HashMap<GC::Ptr<CSSRule const>, StyleEngineRuleID>* non_author_rule_ids, StyleComputer& style_computer, CascadeOrigin cascade_origin, Utf16FlyString const& layer_name)
{
    if (non_author_rule_ids)
        non_author_rule_ids->set(&rule, rule_id);
    else
        rule.set_style_engine_rule_id(rule_id);
    style_computer.register_style_engine_rule_identity(rule_id, rule);

    // The way back from the identity to what the rule contributes. Only a rule that carries a
    // declaration can be reported as a match, and only those are worth a way back.
    if (rule.type() != CSSRule::Type::Style && rule.type() != CSSRule::Type::NestedDeclarations)
        return;
    GC::Ptr<CSSContainerRule const> container_rule;
    for (auto ancestor = rule.parent_rule(); ancestor; ancestor = ancestor->parent_rule()) {
        if (auto const* container = as_if<CSSContainerRule>(*ancestor)) {
            container_rule = container;
            break;
        }
    }
    style_computer.register_style_engine_rule_target(rule, StyleEngineRuleTarget {
                                                               .rule = &rule,
                                                               .container_rule = container_rule,
                                                               .qualified_layer_name = layer_name,
                                                               .cascade_origin = cascade_origin,
                                                           });
}

void record_cascade_layer_order(DOM::Document& document, TreeScopeID tree_scope, ReadonlySpan<Utf16FlyString> qualified_names_in_order)
{
    document.flush_deferred_style_change_event();
    Vector<u32> layers;
    layers.ensure_capacity(qualified_names_in_order.size());
    auto& style_engine = document.style_computer().style_engine();
    for (auto const& name : qualified_names_in_order)
        layers.unchecked_append(name.is_empty() ? 0 : style_engine.intern_atom(name).value());
    style_engine.set_layer_order(tree_scope, layers);
}

static Utf16FlyString qualified_layer_name_within(Utf16FlyString const& parent, Utf16FlyString const& name)
{
    if (parent.is_empty())
        return name;
    Utf16StringBuilder builder;
    builder.append(parent);
    builder.append_ascii('.');
    builder.append(name);
    auto qualified_name = builder.to_string();
    return Utf16FlyString::from_utf16(qualified_name.utf16_view());
}

// https://drafts.csswg.org/css-cascade-6/#scope-atrule
// An `@scope` with no `<scope-start>` roots at an element rather than at a selector, so nothing the
// compiler reads says where it is. Resolving it here is the same walk the matcher makes, and the
// engine names the answer by identity.
static constexpr StyleNodeID implicit_scope_root_of_the_containing_tree { 0xffffffff };

static StyleNodeID implicit_scope_root_of(CSSRule const& scope_rule)
{
    auto* owner_style_sheet = scope_rule.parent_style_sheet();
    if (!owner_style_sheet)
        return no_style_node;

    // If no <scope-start> is specified, the scoping root is the parent element of the owner node of
    // the stylesheet where the @scope rule is defined.
    if (auto* owner_node = const_cast<CSSStyleSheet&>(*owner_style_sheet).owner_node()) {
        if (auto parent = owner_node->parent_element())
            return parent->style_node_id();
    }

    // If no such element exists and the containing node tree is a shadow tree, then the scoping root
    // is the shadow host; otherwise it is the root of the containing node tree. Which of those holds
    // is a property of the scope a sheet is being asked in rather than of the sheet - one
    // constructed sheet can be adopted into several - so the engine resolves it while matching.
    return implicit_scope_root_of_the_containing_tree;
}

static void append_scope_level(CSSRule const& owner, Optional<SelectorList> const& start, Optional<SelectorList> const& end, Vector<void const*>& scope_roots, Vector<void const*>& scope_limits, StyleEngine::ScopeLevels& scope_levels)
{
    u32 roots_here = 0;
    u32 limits_here = 0;
    if (start.has_value()) {
        for (auto const& selector : *start) {
            scope_roots.append(&selector->rust_selector());
            ++roots_here;
        }
    }
    if (end.has_value()) {
        for (auto const& selector : *end) {
            scope_limits.append(&selector->rust_selector());
            ++limits_here;
        }
    }
    // Each `@scope` is one level. An omitted start uses its implicit root, while an explicit start
    // that transforms to an empty list remains a level that matches no roots.
    scope_levels.root_counts.append(roots_here);
    scope_levels.limit_counts.append(limits_here);
    scope_levels.implicit_roots.append(!start.has_value() ? implicit_scope_root_of(owner) : no_style_node);
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
    HashMap<GC::Ptr<CSSRule const>, StyleEngineRuleID>* non_author_rule_ids { nullptr };
    Vector<void const*> scope_roots;
    Vector<void const*> scope_limits;
    StyleEngine::ScopeLevels scope_levels;
    Utf16FlyString layer_name;
    CascadeOrigin cascade_origin { CascadeOrigin::Author };
    bool conditions_hold { true };
    bool in_a_layer { false };
    bool gated_by_container_query { false };
};

static void compile_rules_into(RuleCompilationContext const& context, CSSRule& rule)
{
    auto& style_engine = context.style_engine;
    auto const sheet_handle = context.sheet_handle;
    auto const before_rule = context.before_rule;
    auto const& document = *context.document;
    auto& style_computer = *context.style_computer;
    auto* non_author_rule_ids = context.non_author_rule_ids;
    auto const& scope_roots = context.scope_roots;
    auto const& scope_limits = context.scope_limits;
    auto const& scope_levels = context.scope_levels;
    auto const& layer_name = context.layer_name;
    auto const cascade_origin = context.cascade_origin;
    auto const conditions_hold = context.conditions_hold;
    auto const in_a_layer = context.in_a_layer;
    auto const gated_by_container_query = context.gated_by_container_query;
    auto record_initial_conditions = [&](StyleEngineRuleID rule_id) {
        if (!conditions_hold)
            style_engine.set_rule_conditions_hold(rule_id, false);
    };

    // A block of nested declarations is a rule of its own: the cascade matches it by the selector its
    // position implies, which is its parent rule's selector, or `:where(:scope)` directly inside a
    // scope.
    SelectorList const* matching_selectors = nullptr;
    if (auto* style_rule = as_if<CSSStyleRule>(rule))
        matching_selectors = &style_rule->absolutized_selectors();
    else if (auto* nested_declarations = as_if<CSSNestedDeclarations>(rule))
        matching_selectors = &nested_declarations->absolutized_selectors();

    if (matching_selectors) {
        Vector<void const*> selectors;
        selectors.ensure_capacity(matching_selectors->size());
        for (auto const& selector : *matching_selectors)
            selectors.unchecked_append(&selector->rust_selector());
        auto namespaces = namespace_scope_of(style_engine, rule.parent_style_sheet());
        if (!selectors.is_empty()) {
            auto rule_id = style_engine.add_style_rule(sheet_handle, before_rule, selectors, namespaces, scope_roots, scope_limits, scope_levels);
            set_compiled_rule_id(rule, rule_id, non_author_rule_ids, style_computer, cascade_origin, layer_name);
            record_rule_declared_properties(style_engine, rule_id, rule);
            // A container query asks about the element being styled, so two elements matching this
            // rule can disagree about it and the rule's activation cannot answer it once.
            if (gated_by_container_query)
                style_engine.set_rule_gated_by_container_query(rule_id);
            // A rule behind a group whose condition does not hold keeps its identity and its place
            // and decides nothing, which is the same shape a disabled sheet has.
            record_initial_conditions(rule_id);
            // Which layer a rule sits in is part of what the rule is: the cascade compares where
            // it sits, and a change to layer order moves every rule that references it.
            if (in_a_layer) {
                style_engine.set_rule_in_a_layer(rule_id);
                style_engine.set_rule_layer(rule_id, style_engine.intern_atom(layer_name).value());
            }
        }
    }

    // `@font-feature-values` matches nothing either, and unlike `@font-face` it has no consumer index
    // to be found by: it is in the program so that a change to it is an input at all.
    if (is<CSSFontFeatureValuesRule>(rule)) {
        auto rule_id = style_engine.add_font_feature_values_rule(sheet_handle, before_rule);
        set_compiled_rule_id(rule, rule_id, non_author_rule_ids, style_computer, cascade_origin, layer_name);
        record_initial_conditions(rule_id);
        return;
    }

    // A counter style matches nothing, but changes the counter-style environment used by list
    // markers and generated content. Keep its position and layer in the program so attachment,
    // condition, and layer changes are routed like the environment changes they are.
    if (is<CSSCounterStyleRule>(rule)) {
        auto rule_id = style_engine.add_counter_style_rule(sheet_handle, before_rule);
        set_compiled_rule_id(rule, rule_id, non_author_rule_ids, style_computer, cascade_origin, layer_name);
        record_initial_conditions(rule_id);
        if (in_a_layer) {
            style_engine.set_rule_in_a_layer(rule_id);
            style_engine.set_rule_layer(rule_id, style_engine.intern_atom(layer_name).value());
        }
        return;
    }

    // A custom function contributes no declarations and matches nothing: the elements it decides for
    // are the ones that called it.
    if (is<CSSFunctionRule>(rule)) {
        auto rule_id = style_engine.add_function_rule(sheet_handle, before_rule);
        set_compiled_rule_id(rule, rule_id, non_author_rule_ids, style_computer, cascade_origin, layer_name);
        record_initial_conditions(rule_id);
        return;
    }

    // A layer statement contributes no declarations and matches nothing: it fixes the order of the
    // layers other rules sit in. A layer block and an `@import` naming a layer declare their layer on
    // the way in as well. A declaration behind a condition that does not hold declares nothing, so it
    // moves no order.
    if (conditions_hold && declares_a_layer(rule)) {
        style_engine.record_layer_statement(sheet_handle);
        // NB: The layer order is otherwise published lazily with the rule cache, which is after
        //     transaction has already ranked with the old order. Publish it now so the
        //     plan for this very statement ranks with the order it establishes.
        style_computer.document().style_scope().publish_cascade_layer_order();
    }
    if (is<CSSLayerStatementRule>(rule))
        return;

    // An `@property` rule matches nothing either, and unlike keyframes it has no name to be found
    // by: registering a property changes how every element that declares or references it computes.
    // It is in the program so that a change to it is an input at all.
    if (auto* property_rule = as_if<CSSPropertyRule>(rule)) {
        auto rule_id = style_engine.add_property_rule(
            sheet_handle,
            before_rule,
            style_engine.intern_atom(property_rule->name()),
            property_rule->initial_value().has_value());
        set_compiled_rule_id(
            rule,
            rule_id,
            non_author_rule_ids,
            style_computer,
            cascade_origin,
            layer_name);
        record_initial_conditions(rule_id);
        return;
    }

    // A `@keyframes` rule matches nothing, so it is in the program only to be found by the name it
    // declares - which is how a change to it reaches the elements running that animation.
    if (auto* keyframes_rule = as_if<CSSKeyframesRule>(rule)) {
        auto rule_id = style_engine.add_keyframes_rule(
            sheet_handle,
            before_rule,
            style_engine.intern_atom(keyframes_rule->name()));
        set_compiled_rule_id(
            rule,
            rule_id,
            non_author_rule_ids,
            style_computer,
            cascade_origin,
            layer_name);
        record_initial_conditions(rule_id);
        return;
    }

    // An imported sheet's rules belong to the importing sheet's program: they cascade in its place
    // and they are not attached anywhere else, so nothing else would compile them. The import can
    // carry a scope of its own, which constrains everything it brings in.
    if (auto* import_rule = as_if<CSSImportRule>(rule)) {
        auto* imported = import_rule->loaded_style_sheet();
        if (!imported)
            return;
        auto imported_in_a_layer = in_a_layer || import_rule->layer_name().has_value();
        auto imported_layer_name = layer_name;
        // An anonymous `@import ... layer` opens a layer of its own, which the public name cannot
        // tell from any other anonymous one - it is the empty string for all of them. The internal
        // name is what distinguishes them.
        if (auto const& imported_layer = import_rule->internal_layer_name(); imported_layer.has_value())
            imported_layer_name = qualified_layer_name_within(layer_name, *imported_layer);
        auto imported_context = context;
        imported_context.layer_name = imported_layer_name;
        imported_context.conditions_hold = conditions_hold && condition_holds(rule, document);
        imported_context.in_a_layer = imported_in_a_layer;
        if (import_rule->has_scope())
            append_scope_level(*import_rule, import_rule->scope_start_selectors_for_matching(), import_rule->scope_end_selectors_for_matching(), imported_context.scope_roots, imported_context.scope_limits, imported_context.scope_levels);
        for (size_t index = 0; index < imported->rules().length(); ++index) {
            if (auto child = imported->rules().item(index))
                compile_rules_into(imported_context, *child);
        }
        return;
    }

    if (auto* grouping_rule = as_if<CSSGroupingRule>(rule)) {
        // A scope is a constraint on every rule inside it, and scopes nest. Carrying the roots down
        // is what puts the scope's own selectors into the compiled entries, and therefore into
        // routing: a class that moves an element in or out of a scope has to reach the rules the
        // scope holds.
        auto nested_context = context;
        if (auto* scope_rule = as_if<CSSScopeRule>(rule)) {
            // A scope limit is not a constraint the compiled entry can express, but an element
            // starting or stopping being one moves the scope membership of itself and everything
            // under it, and that has to reach the rules the scope holds.
            append_scope_level(*scope_rule, scope_rule->start_selectors_for_matching(), scope_rule->end_selectors_for_matching(), nested_context.scope_roots, nested_context.scope_limits, nested_context.scope_levels);
        }
        // `@media` and `@supports` gate every rule inside them. The gate is activation, not
        // existence: the rules are still compiled and still hold their positions, so a condition
        // coming true later needs nothing more than to be said.
        nested_context.conditions_hold = conditions_hold && condition_holds(rule, document);
        if (auto* layer_block = as_if<CSSLayerBlockRule>(rule))
            nested_context.layer_name = qualified_layer_name_within(layer_name, layer_block->internal_name());
        nested_context.in_a_layer = in_a_layer || is<CSSLayerBlockRule>(rule);
        nested_context.gated_by_container_query = gated_by_container_query || is<CSSContainerRule>(rule);

        for (size_t index = 0; index < grouping_rule->css_rules().length(); ++index) {
            if (auto child = grouping_rule->css_rules().item(index))
                compile_rules_into(nested_context, *child);
        }
    }
}

// A sheet's rules in the order they are compiled, which is the order they cascade in.
static void collect_rules_in_cascade_order(CSSRuleList& rules, Vector<GC::Ref<CSSRule>>& out)
{
    for (size_t index = 0; index < rules.length(); ++index) {
        auto rule = rules.item(index);
        if (!rule)
            continue;
        out.append(*rule);
        if (auto* import_rule = as_if<CSSImportRule>(*rule)) {
            if (auto* imported = import_rule->loaded_style_sheet())
                collect_rules_in_cascade_order(imported->rules(), out);
        } else if (auto* grouping_rule = as_if<CSSGroupingRule>(*rule)) {
            collect_rules_in_cascade_order(grouping_rule->css_rules(), out);
        }
    }
}

// The engine rule that a rule compiled at `rule`'s position would come before, or 0 when nothing
// follows it. Rules inside `rule` are skipped, because they are about to be compiled with it.
static StyleEngineRuleID successor_of(StyleComputer const& style_computer, CSSStyleSheet& sheet, CSSRule& rule)
{
    Vector<GC::Ref<CSSRule>> order;
    collect_rules_in_cascade_order(sheet.rules(), order);
    auto position = order.find_first_index_if([&](auto const& entry) { return entry.ptr() == &rule; });
    if (!position.has_value())
        return 0;

    Vector<GC::Ref<CSSRule>> inside;
    if (auto* import_rule = as_if<CSSImportRule>(rule)) {
        if (auto* imported = import_rule->loaded_style_sheet())
            collect_rules_in_cascade_order(imported->rules(), inside);
    } else if (auto* grouping_rule = as_if<CSSGroupingRule>(rule)) {
        collect_rules_in_cascade_order(grouping_rule->css_rules(), inside);
    }
    for (size_t index = *position + 1 + inside.size(); index < order.size(); ++index) {
        if (auto rule_id = style_computer.style_engine_rule_id_for(order[index]); rule_id != 0)
            return rule_id;
    }
    return 0;
}

static GC::RootVector<GC::Ref<CSSRule>> enclosing_rules(CSSRule& rule)
{
    GC::RootVector<GC::Ref<CSSRule>> enclosing;
    for (auto* ancestor = rule.parent_rule(); ancestor; ancestor = ancestor->parent_rule())
        enclosing.append(*ancestor);
    for (auto* sheet = rule.parent_style_sheet(); sheet;) {
        auto owner = sheet->owner_rule();
        if (!owner)
            break;
        enclosing.append(*owner);
        for (auto* ancestor = owner->parent_rule(); ancestor; ancestor = ancestor->parent_rule())
            enclosing.append(*ancestor);
        sheet = owner->parent_style_sheet();
    }
    return enclosing;
}

// The scope a rule sits in, which its enclosing `@scope` rules and importing `@import scope()`
// decide. A rule arriving on its own has to be compiled with the same scope the rules around it
// were, or it would apply where they do not.
static void collect_enclosing_scope(GC::RootVector<GC::Ref<CSSRule>> const& enclosing, Vector<void const*>& scope_roots, Vector<void const*>& scope_limits, StyleEngine::ScopeLevels& scope_levels)
{
    // Outermost first, so the scopes read the way they nest.
    for (size_t index = enclosing.size(); index > 0; --index) {
        auto& ancestor = *enclosing[index - 1];
        Optional<SelectorList> const* start = nullptr;
        Optional<SelectorList> const* end = nullptr;
        if (auto* scope_rule = as_if<CSSScopeRule>(ancestor)) {
            start = &scope_rule->start_selectors_for_matching();
            end = &scope_rule->end_selectors_for_matching();
        } else if (auto* import_rule = as_if<CSSImportRule>(ancestor); import_rule && import_rule->has_scope()) {
            start = &import_rule->scope_start_selectors_for_matching();
            end = &import_rule->scope_end_selectors_for_matching();
        }
        if (!start && !end)
            continue;
        append_scope_level(ancestor, *start, *end, scope_roots, scope_limits, scope_levels);
    }
}

// The sheet a rule's compiled rules belong to. An imported sheet's rules cascade in the importing
// sheet's program, so the handle to compile into is the outermost sheet's. A constructed sheet is
// always its own engine sheet: its ids are held per adopting document, so its raw id member being 0
// does not mean its rules live in another sheet's program.
static CSSStyleSheet* owning_compiled_sheet(CSSRule& rule)
{
    auto* sheet = rule.parent_style_sheet();
    while (sheet && !sheet->constructed() && sheet->style_engine_sheet_id() == 0) {
        auto owner = sheet->owner_rule();
        if (!owner)
            return nullptr;
        sheet = owner->parent_style_sheet();
    }
    return sheet;
}

// A constructed sheet compiles once per adopting document, so a mutation to it has to be replayed
// into every document engine holding a copy; every other sheet has exactly one owning document.
static void for_each_document_with_engine_copy(CSSStyleSheet& sheet, auto const& callback)
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

static void flush_deferred_style_change_events_for_sheet(CSSStyleSheet& sheet)
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

// Whether the conditions of every group a rule sits inside hold. A rule compiled on its own has to
// be told the same thing the whole-sheet walk would have told it.
static bool enclosing_conditions_hold(GC::RootVector<GC::Ref<CSSRule>> const& enclosing, DOM::Document const& document)
{
    for (auto const& ancestor : enclosing) {
        if (!condition_holds(*ancestor, document))
            return false;
    }
    return true;
}

// Reconstruct the group context that the whole-sheet walk would have carried to a rule compiled on
// its own after a CSSOM insertion.
static void collect_enclosing_group_context(GC::RootVector<GC::Ref<CSSRule>> const& enclosing, RuleCompilationContext& context)
{
    bool in_a_layer = false;
    for (auto const& ancestor : enclosing) {
        if (is<CSSLayerBlockRule>(*ancestor))
            in_a_layer = true;
        if (is<CSSContainerRule>(*ancestor))
            context.gated_by_container_query = true;
        if (auto* import_rule = as_if<CSSImportRule>(*ancestor); import_rule && import_rule->layer_name().has_value())
            in_a_layer = true;
    }
    for (size_t index = enclosing.size(); index > 0; --index) {
        auto& ancestor = *enclosing[index - 1];
        if (auto* layer = as_if<CSSLayerBlockRule>(ancestor))
            context.layer_name = qualified_layer_name_within(context.layer_name, layer->internal_name());
        else if (auto* import_rule = as_if<CSSImportRule>(ancestor); import_rule && import_rule->internal_layer_name().has_value())
            context.layer_name = qualified_layer_name_within(context.layer_name, *import_rule->internal_layer_name());
    }
    context.in_a_layer = in_a_layer;
}

// A rule arrived in one document's engine. Compile it, and everything it brings with it, into the
// position it holds there.
static void record_style_rule_inserted_in(CSSRule& rule, CSSStyleSheet& sheet, DOM::Document& document)
{
    document.flush_deferred_style_change_event();
    auto& style_computer = document.style_computer();
    auto sheet_id = style_computer.style_engine_sheet_id_for(sheet);
    if (sheet_id == 0)
        return;

    document.bump_style_environment_version();

    Vector<void const*> scope_roots;
    Vector<void const*> scope_limits;
    StyleEngine::ScopeLevels scope_levels;
    auto enclosing = enclosing_rules(rule);
    collect_enclosing_scope(enclosing, scope_roots, scope_limits, scope_levels);
    RuleCompilationContext context {
        style_computer.style_engine(),
        sheet_id,
        successor_of(style_computer, sheet, rule),
        document,
        style_computer
    };
    if (sheet.constructed())
        context.non_author_rule_ids = &style_computer.constructed_rule_ids();
    context.scope_roots = move(scope_roots);
    context.scope_limits = move(scope_limits);
    context.scope_levels = move(scope_levels);
    context.conditions_hold = enclosing_conditions_hold(enclosing, document);
    collect_enclosing_group_context(enclosing, context);
    compile_rules_into(context, rule);
}

// A rule arrived. Compile it, and everything it brings with it, into the position it holds.
void record_style_rule_inserted(CSSRule& rule)
{
    auto* sheet = owning_compiled_sheet(rule);
    if (!sheet)
        return;
    for_each_document_with_engine_copy(*sheet, [&](DOM::Document& document) {
        record_style_rule_inserted_in(rule, *sheet, document);
    });
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
    record_style_rule_removed(*sheet, rule);
}

void record_style_rule_removed(CSSStyleSheet& sheet_it_left, CSSRule& rule)
{
    Vector<GC::Ref<CSSRule>> removed;
    removed.append(rule);
    if (auto* import_rule = as_if<CSSImportRule>(rule)) {
        if (auto* imported = import_rule->loaded_style_sheet())
            collect_rules_in_cascade_order(imported->rules(), removed);
    } else if (auto* grouping_rule = as_if<CSSGroupingRule>(rule)) {
        collect_rules_in_cascade_order(grouping_rule->css_rules(), removed);
    }

    bool any_engine_heard = false;
    for_each_document_with_engine_copy(sheet_it_left, [&](DOM::Document& document) {
        any_engine_heard = true;
        document.flush_deferred_style_change_event();
        document.bump_style_environment_version();

        auto& style_computer = document.style_computer();
        auto& style_engine = style_computer.style_engine();
        for (auto& entry : removed) {
            // A layer declaration leaving reorders the layers as much as one arriving does, and it holds no
            // rule identity that removing would carry the change for.
            if (declares_a_layer(entry)) {
                if (auto sheet_id = style_computer.style_engine_sheet_id_for(sheet_it_left); sheet_id != 0)
                    style_engine.record_layer_statement(sheet_id);
                document.style_scope().publish_cascade_layer_order();
            }
            if (auto rule_id = style_computer.style_engine_rule_id_for(entry); rule_id != 0) {
                style_computer.invalidate_parsed_substitutions_for_rule(rule_id);
                style_engine.remove_rule(rule_id);
                style_computer.non_author_rule_ids().remove(entry.ptr());
                style_computer.constructed_rule_ids().remove(entry.ptr());
            }
        }
    });

    // The identities are cleared after every engine copy heard the removal, so the loop above can
    // still resolve them.
    if (!any_engine_heard)
        return;
    for (auto& entry : removed)
        entry->set_style_engine_rule_id(0);
}

// The selectors a rule matches by, replaced in place. A rule that compiled to nothing has no
// identity to replace, so it arrives instead.
static void replace_matching_selectors(CSSRule& rule, DOM::Document& document)
{
    // A rule that matches nothing by a selector of its own holds no identity to replace, and it is
    // not one that arrives either: re-inserting a grouping rule compiles everything nested inside it
    // a second time, leaving the rules it already had in the engine with nobody to update them.
    SelectorList const* matching_selectors = nullptr;
    if (auto* style_rule = as_if<CSSStyleRule>(rule))
        matching_selectors = &style_rule->absolutized_selectors();
    else if (auto* nested_declarations = as_if<CSSNestedDeclarations>(rule))
        matching_selectors = &nested_declarations->absolutized_selectors();
    if (!matching_selectors)
        return;

    auto& style_computer = document.style_computer();
    auto rule_id = style_computer.style_engine_rule_id_for(rule);
    if (rule_id == 0) {
        if (auto* sheet = owning_compiled_sheet(rule))
            record_style_rule_inserted_in(rule, *sheet, document);
        return;
    }

    auto& style_engine = style_computer.style_engine();
    Vector<void const*> selectors;
    selectors.ensure_capacity(matching_selectors->size());
    for (auto const& selector : *matching_selectors)
        selectors.unchecked_append(&selector->rust_selector());
    auto namespaces = namespace_scope_of(style_engine, rule.parent_style_sheet());
    if (selectors.is_empty())
        return;

    Vector<void const*> scope_roots;
    Vector<void const*> scope_limits;
    StyleEngine::ScopeLevels scope_levels;
    auto enclosing = enclosing_rules(rule);
    collect_enclosing_scope(enclosing, scope_roots, scope_limits, scope_levels);
    style_engine.replace_style_rule_selectors(rule_id, selectors, namespaces, scope_roots, scope_limits, scope_levels);
}

// A rule kept its place and its declarations, and changed what it selects.
void record_style_rule_selector_changed(CSSStyleRule& rule)
{
    auto* sheet = owning_compiled_sheet(rule);
    if (!sheet)
        return;

    // A nested rule's selector is written relative to the one it sits in, so a rule's selector
    // changing changes what everything nested inside it matches as well.
    Vector<GC::Ref<CSSRule>> affected;
    affected.append(rule);
    for (size_t index = 0; index < affected.size(); ++index) {
        if (auto* grouping_rule = as_if<CSSGroupingRule>(*affected[index])) {
            for (size_t child = 0; child < grouping_rule->css_rules().length(); ++child) {
                if (auto nested = grouping_rule->css_rules().item(child))
                    affected.append(*nested);
            }
        }
    }

    for_each_document_with_engine_copy(*sheet, [&](DOM::Document& document) {
        document.flush_deferred_style_change_event();
        for (auto& affected_rule : affected)
            replace_matching_selectors(*affected_rule, document);
    });
}

// A rule kept its place and its selector, and changed what it declares.
void record_style_rule_declarations_changed(CSSRule& rule)
{
    // A keyframe block is not a rule the engine holds: what the cascade sees is the `@keyframes` rule
    // it belongs to, so editing one keyframe is that rule's declarations changing.
    auto* changed_rule = &rule;
    if (is<CSSKeyframeRule>(rule)) {
        changed_rule = rule.parent_rule();
        if (!changed_rule)
            return;
    }

    auto& rule_to_report = *changed_rule;
    auto* sheet = owning_compiled_sheet(rule_to_report);
    if (!sheet)
        return;

    for_each_document_with_engine_copy(*sheet, [&](DOM::Document& document) {
        document.flush_deferred_style_change_event();
        auto& style_computer = document.style_computer();
        auto rule_id = style_computer.style_engine_rule_id_for(rule_to_report);
        if (rule_id == 0)
            return;

        // Every style input record naming this block names it by its identity, which has not moved.
        document.bump_style_environment_version();

        auto& style_engine = style_computer.style_engine();
        style_computer.invalidate_parsed_substitutions_for_rule(rule_id);
        style_engine.record_rule_declarations_changed(rule_id, style_engine.next_declaration_block_version());
        // Which properties the rule declares is part of what changed: an edit that adds or drops one
        // changes which properties it can win.
        record_rule_declared_properties(style_engine, rule_id, rule_to_report);
    });
}

// `replace()` swaps a sheet's whole rule list, so there is nothing of the old one to keep.
void record_stylesheet_rules_replaced(CSSStyleSheet& sheet)
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
        if (sheet.constructed())
            context.non_author_rule_ids = &style_computer.constructed_rule_ids();
        for (size_t index = 0; index < sheet.rules().length(); ++index) {
            if (auto rule = sheet.rules().item(index))
                compile_rules_into(context, *rule);
        }
        style_engine.finish_sheet_rules_replacement(sheet_id);
    });
}

void record_stylesheet_attached(CSSStyleSheet& sheet, DOM::Node& document_or_shadow_root, CSSStyleSheet* before)
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
        style_engine.set_sheet_conditions_hold(sheet_id, !sheet.disabled() && sheet.media()->matches());
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
    if (sheet.constructed())
        context.non_author_rule_ids = &style_computer.constructed_rule_ids();
    for (size_t index = 0; index < sheet.rules().length(); ++index) {
        if (auto rule = sheet.rules().item(index))
            compile_rules_into(context, *rule);
    }
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
    document.flush_deferred_style_change_event();
    auto& style_computer = document.style_computer();
    auto& style_scope = document.style_scope();

    publish_document_kind(document);

    Vector<GC::Ref<CSSStyleSheet>> sheets;
    Vector<StyleEngineFFI::FfiCascadeOrigin> origins;
    for (auto origin : { CascadeOrigin::UserAgent, CascadeOrigin::User }) {
        style_scope.for_each_stylesheet(origin, [&](CSSStyleSheet& sheet) {
            sheets.append(sheet);
            origins.append(origin == CascadeOrigin::UserAgent ? StyleEngineFFI::FfiCascadeOrigin::UserAgent : StyleEngineFFI::FfiCascadeOrigin::User);
        });
    }

    auto& recorded = style_computer.non_author_style_sheets();
    if (recorded.size() == sheets.size()) {
        bool unchanged = true;
        for (size_t index = 0; index < sheets.size(); ++index)
            unchanged &= recorded[index].sheet == sheets[index];
        if (unchanged)
            return;
    }

    auto& style_engine = style_computer.style_engine();
    for (auto const& entry : recorded)
        style_engine.detach_sheet(entry.sheet_id, document_tree_scope);
    recorded.clear();
    auto& non_author_rule_ids = style_computer.non_author_rule_ids();
    non_author_rule_ids.clear();

    // These origins cascade before every author sheet, so each is inserted ahead of the first one
    // rather than appended. Inserting each new sheet before that same successor keeps them in the
    // order they were collected.
    SheetID first_author_sheet;
    for (auto const& sheet : document.style_sheets().sheets()) {
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
        context.non_author_rule_ids = &non_author_rule_ids;
        context.cascade_origin = origins[index] == StyleEngineFFI::FfiCascadeOrigin::UserAgent ? CascadeOrigin::UserAgent : CascadeOrigin::User;
        for (size_t rule_index = 0; rule_index < sheets[index]->rules().length(); ++rule_index) {
            if (auto rule = sheets[index]->rules().item(rule_index))
                compile_rules_into(context, *rule);
        }
        recorded.append({ sheets[index], sheet_id });
    }
}

// A group's condition can come true or stop being true without the sheet's own media moving - a
// viewport change is one `@media` becoming false and another becoming true. Each rule hears the
// state it is now in, and the engine rejects the ones that did not move.
static void record_rule_conditions_in(StyleComputer& style_computer, CSSRule& rule, bool conditions_hold, DOM::Document const& document)
{
    auto& style_engine = style_computer.style_engine();
    if (auto rule_id = style_computer.style_engine_rule_id_for(rule); rule_id != 0)
        style_engine.set_rule_conditions_hold(rule_id, conditions_hold);

    if (auto* import_rule = as_if<CSSImportRule>(rule)) {
        if (auto* imported = import_rule->loaded_style_sheet()) {
            auto imported_conditions_hold = conditions_hold && condition_holds(rule, document);
            for (size_t index = 0; index < imported->rules().length(); ++index) {
                if (auto child = imported->rules().item(index))
                    record_rule_conditions_in(style_computer, *child, imported_conditions_hold, document);
            }
        }
        return;
    }

    if (auto* grouping_rule = as_if<CSSGroupingRule>(rule)) {
        auto nested = conditions_hold && condition_holds(rule, document);
        for (size_t index = 0; index < grouping_rule->css_rules().length(); ++index) {
            if (auto child = grouping_rule->css_rules().item(index))
                record_rule_conditions_in(style_computer, *child, nested, document);
        }
    }
}

static CSSStyleSheet* owning_engine_sheet(CSSStyleSheet& sheet)
{
    // A constructed sheet is always its own engine sheet; its per-document ids make the raw member 0
    // without its rules living in any other sheet's program.
    if (sheet.constructed())
        return &sheet;
    auto* engine_sheet = &sheet;
    while (engine_sheet->style_engine_sheet_id() == 0) {
        auto owner = engine_sheet->owner_rule();
        if (!owner)
            return nullptr;
        engine_sheet = owner->parent_style_sheet();
        if (!engine_sheet)
            return nullptr;
    }
    return engine_sheet;
}

void record_stylesheet_rule_conditions(CSSStyleSheet& sheet)
{
    auto* engine_sheet = owning_engine_sheet(sheet);
    if (!engine_sheet)
        return;
    for_each_document_with_engine_copy(*engine_sheet, [&](DOM::Document& document) {
        record_stylesheet_rule_conditions(sheet, document);
    });
}

void record_stylesheet_rule_conditions(CSSStyleSheet& sheet, DOM::Document& document)
{
    document.flush_deferred_style_change_event();
    auto& style_computer = document.style_computer();
    for (size_t index = 0; index < sheet.rules().length(); ++index) {
        if (auto rule = sheet.rules().item(index))
            record_rule_conditions_in(style_computer, *rule, true, document);
    }
}

void record_stylesheet_conditions(CSSStyleSheet& sheet, DOM::Node& document_or_shadow_root, bool conditions_hold)
{
    document_or_shadow_root.document().flush_deferred_style_change_event();
    auto* engine_sheet = owning_engine_sheet(sheet);
    if (!engine_sheet)
        return;
    auto& style_computer = document_or_shadow_root.document().style_computer();
    auto sheet_id = style_computer.style_engine_sheet_id_for(*engine_sheet);
    if (sheet_id == 0)
        return;
    style_computer.style_engine().set_sheet_conditions_hold(sheet_id, conditions_hold);
}

void record_stylesheet_detached(CSSStyleSheet& sheet, DOM::Node& document_or_shadow_root)
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
