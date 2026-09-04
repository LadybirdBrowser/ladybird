/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <LibWeb/CSS/PseudoClass.h>
#include <LibWeb/CSS/StyleProperty.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

class StyleEngine;

// Whether declaring this property can provide the parameters for a CSS transition.
WEB_API bool property_defines_a_css_transition(PropertyID);

// Settle a style-change boundary deferred by a geometry read before mutating a rule's declaration
// block. Constructed sheets can have copies in multiple document engines.
WEB_API void flush_deferred_style_change_events_for_rule(CSSRule&);

// Translates DOM and CSSOM mutations into StyleEngine's typed semantic inputs.
//
// These are recording calls, not evaluation: each one appends to the batch that crosses into the
// engine at the next style flush. They are deliberately cheap and total -- an element with no style
// node identity records nothing at all, which is what keeps disconnected and never-styled content
// free.
//
// Called once a node has been linked into a connected tree. Allocates the element's style node
// identity if it does not have one yet.
WEB_API void record_element_connected(DOM::Element&);
WEB_API void prepare_style_nodes_for_subtree(DOM::Node&);
WEB_API void publish_pending_element_features(StyleEngine&, StyleComputer&);
WEB_API void publish_required_attribute_value_texts(StyleEngine&, StyleComputer&);

// Populate an isolated engine with the current facts of a DOM tree. The callback receives the temporary identity
// assigned to each element; no identity or transaction in the document's
// resident engine is changed.
WEB_API void configure_isolated_selector_query_engine(StyleEngine&, DOM::Document&);
WEB_API void populate_isolated_selector_query_engine(StyleEngine&, DOM::ParentNode&, Function<void(GC::Ref<DOM::Element>, StyleNodeID)> const&);

// Tell the document's engine whether this is an HTML document. Selectors compile against that fact,
// so it is published before any rule compiles; a selector query compiled by an early script can run
// before the first sheet attaches, and has to say it itself.
WEB_API void record_document_kind(DOM::Document&);

// Called while the subtree is still linked, so its old relations are still readable.
WEB_API void record_subtree_disconnecting(DOM::Node&);

// Report that an element moved without leaving the tree. `moveBefore()` keeps the element's state
// and its identity, so nothing disconnects and nothing connects, and only its relations move.
WEB_API void record_element_moved(DOM::Element&, DOM::Node* old_parent, DOM::Element* old_previous_sibling, DOM::Element* old_next_sibling);

// A slottable's assigned slot is its parent in the flat tree, and a slot's name changing reassigns
// it there without any DOM mutation. Nothing else says so: the element did not move, so no tree
// delta carries it.
WEB_API void record_element_assigned_slot_changed(DOM::Element&, DOM::Element* old_slot);

// Called once every element of a shadow tree has recorded its own removal, so nothing still names
// the root as a parent. A shadow root's identity follows its host's lifetime: keeping it across a
// move to another document would name an identity that document's engine never minted.
WEB_API void record_shadow_root_disconnecting(DOM::ShadowRoot&);
WEB_API void record_shadow_root_connected(DOM::ShadowRoot&);

// Report the shadow parts an element exposes. A part is a fact about the element like a class is.
WEB_API void record_element_parts_changed(DOM::Element&);

// The custom states an element is in, which `:state()` tests. A state is a named fact about one
// element like a class is, so it is published as a set and the engine journals what moved.
WEB_API void record_element_custom_states_changed(DOM::Element&);

// The element's resolved language and directionality, which `:lang()` and `:dir()` test. Both
// inherit, so a change on one element is a change for its whole subtree, and each descendant
// publishes the value it now resolves to.
WEB_API void record_element_language_and_directionality(DOM::Element&);
WEB_API void record_element_directionality(DOM::Element&);
WEB_API bool record_element_presentational_hint_properties(DOM::Element&, ReadonlySpan<StyleProperty>);
WEB_API void record_element_animation_names(DOM::Element&, ReadonlySpan<Utf16FlyString>);
WEB_API void record_element_custom_property_names(DOM::Element&, ReadonlySpan<Utf16FlyString>, bool uses_unnamed, bool uses_custom_functions);

// The same index, from the environments the element and its pseudo-elements resolved to, plus
// names read outside substitution (for example by style queries). The names each environment
// declares are interned into engine identities once for that environment.
WEB_API void record_element_custom_property_names(DOM::Element&, CustomPropertyData const*, ReadonlySpan<RefPtr<CustomPropertyData const>> pseudo_element_data, ReadonlySpan<Utf16FlyString> references, bool uses_unnamed, bool uses_custom_functions);

// Report that a child of an element arrived, left, or changed in a way that can move whether the
// element is empty. `counted_before` and `counts_after` say whether that child is one `:empty` counts:
// an element child always counts, a text child counts while its data is not empty, and anything else
// never does. The element's other children decide the rest, so a change that moves nothing says
// nothing. A text node connects no element, so nothing but this says it.
WEB_API void record_element_emptiness_changed(DOM::Element&, DOM::Node const& changing_child, bool counted_before, bool counts_after);

// Called for each element whose pseudo-class state actually changed, with the value it changed to.
// Only the states StyleEngine models as facts are recorded; the rest arrive when it models them.
WEB_API bool can_record_element_state_change(DOM::Element&);
WEB_API void record_element_state_changed(DOM::Element&, PseudoClass, bool new_value);

// Called before each style flush. The user-agent and user origins have no sheet list to announce
// themselves from, so the engine is told about them from here.
WEB_API void record_non_author_stylesheets(DOM::Document&);

// Called once a sheet has taken its place in the sheet list, so its successor is known.
WEB_API void record_stylesheet_attached(CSSStyleSheet&, DOM::Node& document_or_shadow_root, CSSStyleSheet* before);

// The order one tree scope declares its qualified cascade layer names in. Rules retain the interned
// names, while StyleEngine derives the scope-local ranks used by every cascade consumer.
WEB_API void record_cascade_layer_order(DOM::Document&, TreeScopeID tree_scope, ReadonlySpan<Utf16FlyString> qualified_names_in_order);
WEB_API TreeScopeID style_engine_tree_scope_for(DOM::Node&);

// A sheet's rules are not immutable, and each of these is one rule moving rather than the sheet
// being rebuilt. Patching what moved is the point: retiring identities that did not change would
// turn an `insertRule` into a program change over the whole sheet.
WEB_API void record_style_rule_inserted(CSSRule&);
WEB_API void record_style_rule_removed(CSSRule&);
WEB_API void record_style_rule_removed(CSSStyleSheet&, CSSRule&);
WEB_API void record_style_rule_selector_changed(CSSStyleRule&);
WEB_API void record_style_rule_declarations_changed(CSSRule&);

// `replace()` is the exception: it swaps the whole rule list, so there is nothing to keep.
WEB_API void record_stylesheet_rules_replaced(CSSStyleSheet&);
WEB_API void record_stylesheet_detached(CSSStyleSheet&, DOM::Node& document_or_shadow_root);

// Called once a sheet's media queries have been evaluated.
WEB_API void record_stylesheet_conditions(CSSStyleSheet&, DOM::Node& document_or_shadow_root, bool conditions_hold);
WEB_API void record_stylesheet_rule_conditions(CSSStyleSheet&);
WEB_API void record_stylesheet_rule_conditions(CSSStyleSheet&, DOM::Document&);

WEB_API void record_element_id_changed(DOM::Element&, Optional<Utf16FlyString> const& old_value, Optional<Utf16FlyString> const& new_value);
WEB_API void record_element_class_list_changed(DOM::Element&, Vector<Utf16FlyString> const& old_classes, Vector<Utf16FlyString> const& new_classes);
WEB_API void record_element_attribute_changed(DOM::Element&, Utf16FlyString const& name, Optional<Utf16FlyString> const& namespace_uri, Optional<Utf16String> const& old_value, Optional<Utf16String> const& new_value);

// Called when a declaration block the element itself sources has changed: its inline style, its
// presentational hints, or its SVG presentation attributes. What that changes is which declaration
// wins on this element, never which elements match, so it reaches the element and nothing else.
enum class ElementDeclarationKind : u8 {
    InlineStyle,
    PresentationalHint,
    SvgPresentationAttribute,
};
WEB_API void record_element_declarations_changed(DOM::Element&, ElementDeclarationKind, bool had_declarations, bool has_declarations);

}
