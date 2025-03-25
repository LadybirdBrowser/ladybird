/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibWeb/ARIA/ARIAMixin.h>
#include <LibWeb/ARIA/AttributeNames.h>
#include <LibWeb/Animations/Animatable.h>
#include <LibWeb/Bindings/ElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/ShadowRootPrototype.h>
#include <LibWeb/CSS/CascadedProperties.h>
#include <LibWeb/CSS/CountersSet.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/StyleInvalidation.h>
#include <LibWeb/CSS/StyleProperty.h>
#include <LibWeb/DOM/ChildNode.h>
#include <LibWeb/DOM/NonDocumentTypeChildNode.h>
#include <LibWeb/DOM/ParentNode.h>
#include <LibWeb/DOM/QualifiedName.h>
#include <LibWeb/DOM/Slottable.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/EventLoop/Task.h>
#include <LibWeb/HTML/LazyLoadingElement.h>
#include <LibWeb/HTML/ScrollOptions.h>
#include <LibWeb/HTML/TagNames.h>
#include <LibWeb/IntersectionObserver/IntersectionObserver.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::DOM {

struct ShadowRootInit {
    Bindings::ShadowRootMode mode;
    bool delegates_focus = false;
    Bindings::SlotAssignmentMode slot_assignment { Bindings::SlotAssignmentMode::Named };
    bool clonable = false;
    bool serializable = false;
};

struct GetHTMLOptions {
    bool serializable_shadow_roots { false };
    Vector<GC::Root<ShadowRoot>> shadow_roots {};
};

// https://w3c.github.io/csswg-drafts/cssom-view-1/#dictdef-scrollintoviewoptions
struct ScrollIntoViewOptions : public HTML::ScrollOptions {
    Bindings::ScrollLogicalPosition block { Bindings::ScrollLogicalPosition::Start };
    Bindings::ScrollLogicalPosition inline_ { Bindings::ScrollLogicalPosition::Nearest };
    Bindings::ScrollIntoViewContainer container { Bindings::ScrollIntoViewContainer::All };
};

// https://drafts.csswg.org/cssom-view-1/#dictdef-checkvisibilityoptions
struct CheckVisibilityOptions {
    bool check_opacity = false;
    bool check_visibility_css = false;
    bool content_visibility_auto = false;
    bool opacity_property = false;
    bool visibility_property = false;
};

// https://html.spec.whatwg.org/multipage/custom-elements.html#upgrade-reaction
// An upgrade reaction, which will upgrade the custom element and contains a custom element definition; or
struct CustomElementUpgradeReaction {
    GC::Root<HTML::CustomElementDefinition> custom_element_definition;
};

// https://html.spec.whatwg.org/multipage/custom-elements.html#callback-reaction
// A callback reaction, which will call a lifecycle callback, and contains a callback function as well as a list of arguments.
struct CustomElementCallbackReaction {
    GC::Root<WebIDL::CallbackType> callback;
    GC::RootVector<JS::Value> arguments;
};

// https://dom.spec.whatwg.org/#concept-element-custom-element-state
// An element’s custom element state is one of "undefined", "failed", "uncustomized", "precustomized", or "custom".
enum class CustomElementState {
    Undefined,
    Failed,
    Uncustomized,
    Precustomized,
    Custom,
};

// https://drafts.csswg.org/css-contain/#proximity-to-the-viewport
// An element that has content-visibility: auto is in one of three states when it comes to its proximity to the viewport:
enum class ProximityToTheViewport {
    // - The element is close to the viewport:
    CloseToTheViewport,
    // - The element is far away from the viewport:
    FarAwayFromTheViewport,
    // - The element’s proximity to the viewport is not determined:
    NotDetermined,
};

class Element
    : public ParentNode
    , public ChildNode<Element>
    , public NonDocumentTypeChildNode<Element>
    , public SlottableMixin
    , public ARIA::ARIAMixin
    , public Animations::Animatable {
    WEB_PLATFORM_OBJECT(Element, ParentNode);

public:
    virtual ~Element() override;

    FlyString const& qualified_name() const { return m_qualified_name.as_string(); }
    FlyString const& html_uppercased_qualified_name() const { return m_html_uppercased_qualified_name; }

    virtual FlyString node_name() const final { return html_uppercased_qualified_name(); }
    FlyString const& local_name() const { return m_qualified_name.local_name(); }

    // NOTE: This is for the JS bindings
    FlyString const& tag_name() const { return html_uppercased_qualified_name(); }

    Optional<FlyString> const& prefix() const { return m_qualified_name.prefix(); }

    void set_prefix(Optional<FlyString> value);

    Optional<String> locate_a_namespace_prefix(Optional<String> const& namespace_) const;

    // NOTE: This is for the JS bindings
    Optional<FlyString> const& namespace_uri() const { return m_qualified_name.namespace_(); }

    bool has_attribute(FlyString const& name) const;
    bool has_attribute_ns(Optional<FlyString> const& namespace_, FlyString const& name) const;
    bool has_attributes() const;

    Optional<String> attribute(FlyString const& name) const { return get_attribute(name); }

    Optional<String> get_attribute(FlyString const& name) const;
    Optional<String> get_attribute_ns(Optional<FlyString> const& namespace_, FlyString const& name) const;
    String get_attribute_value(FlyString const& local_name, Optional<FlyString> const& namespace_ = {}) const;

    Optional<String> lang() const;

    WebIDL::ExceptionOr<void> set_attribute(FlyString const& name, String const& value);

    WebIDL::ExceptionOr<void> set_attribute_ns(Optional<FlyString> const& namespace_, FlyString const& qualified_name, String const& value);
    void set_attribute_value(FlyString const& local_name, String const& value, Optional<FlyString> const& prefix = {}, Optional<FlyString> const& namespace_ = {});
    WebIDL::ExceptionOr<GC::Ptr<Attr>> set_attribute_node(Attr&);
    WebIDL::ExceptionOr<GC::Ptr<Attr>> set_attribute_node_ns(Attr&);

    void append_attribute(FlyString const& name, String const& value);
    void append_attribute(Attr&);
    void remove_attribute(FlyString const& name);
    void remove_attribute_ns(Optional<FlyString> const& namespace_, FlyString const& name);
    WebIDL::ExceptionOr<GC::Ref<Attr>> remove_attribute_node(GC::Ref<Attr>);

    WebIDL::ExceptionOr<bool> toggle_attribute(FlyString const& name, Optional<bool> force);
    size_t attribute_list_size() const;
    NamedNodeMap const* attributes() const { return m_attributes.ptr(); }
    Vector<String> get_attribute_names() const;

    GC::Ptr<Attr> get_attribute_node(FlyString const& name) const;
    GC::Ptr<Attr> get_attribute_node_ns(Optional<FlyString> const& namespace_, FlyString const& name) const;

    DOMTokenList* class_list();

    WebIDL::ExceptionOr<GC::Ref<ShadowRoot>> attach_shadow(ShadowRootInit init);
    WebIDL::ExceptionOr<void> attach_a_shadow_root(Bindings::ShadowRootMode mode, bool clonable, bool serializable, bool delegates_focus, Bindings::SlotAssignmentMode slot_assignment);
    GC::Ptr<ShadowRoot> shadow_root_for_bindings() const;

    WebIDL::ExceptionOr<bool> matches(StringView selectors) const;
    WebIDL::ExceptionOr<DOM::Element const*> closest(StringView selectors) const;

    int client_top() const;
    int client_left() const;
    int client_width() const;
    int client_height() const;
    [[nodiscard]] double current_css_zoom() const;

    void for_each_attribute(Function<void(Attr const&)>) const;

    void for_each_attribute(Function<void(FlyString const&, String const&)>) const;

    bool has_class(FlyString const&, CaseSensitivity = CaseSensitivity::CaseSensitive) const;
    Vector<FlyString> const& class_names() const { return m_classes; }

    // https://html.spec.whatwg.org/multipage/embedded-content-other.html#dimension-attributes
    virtual bool supports_dimension_attributes() const { return false; }

    virtual bool is_presentational_hint(FlyString const&) const { return false; }
    virtual void apply_presentational_hints(GC::Ref<CSS::CascadedProperties>) const { }

    void run_attribute_change_steps(FlyString const& local_name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_);

    CSS::RequiredInvalidationAfterStyleChange recompute_style();
    CSS::RequiredInvalidationAfterStyleChange recompute_inherited_style();

    Optional<CSS::PseudoElement> use_pseudo_element() const { return m_use_pseudo_element; }
    void set_use_pseudo_element(Optional<CSS::PseudoElement> use_pseudo_element) { m_use_pseudo_element = move(use_pseudo_element); }

    GC::Ptr<Layout::NodeWithStyle> layout_node();
    GC::Ptr<Layout::NodeWithStyle const> layout_node() const;

    GC::Ptr<CSS::ComputedProperties> computed_properties() { return m_computed_properties; }
    GC::Ptr<CSS::ComputedProperties const> computed_properties() const { return m_computed_properties; }
    void set_computed_properties(GC::Ptr<CSS::ComputedProperties>);
    GC::Ref<CSS::ComputedProperties> resolved_css_values(Optional<CSS::PseudoElement> = {});

    [[nodiscard]] GC::Ptr<CSS::CascadedProperties> cascaded_properties(Optional<CSS::PseudoElement>) const;
    void set_cascaded_properties(Optional<CSS::PseudoElement>, GC::Ptr<CSS::CascadedProperties>);

    void set_pseudo_element_computed_properties(CSS::PseudoElement, GC::Ptr<CSS::ComputedProperties>);
    GC::Ptr<CSS::ComputedProperties> pseudo_element_computed_properties(CSS::PseudoElement);

    void reset_animated_css_properties();

    GC::Ptr<CSS::CSSStyleProperties> inline_style() { return m_inline_style; }
    GC::Ptr<CSS::CSSStyleProperties const> inline_style() const { return m_inline_style; }
    void set_inline_style(GC::Ptr<CSS::CSSStyleProperties>);

    GC::Ref<CSS::CSSStyleProperties> style_for_bindings();

    CSS::StyleSheetList& document_or_shadow_root_style_sheets();
    ElementByIdMap& document_or_shadow_root_element_by_id_map();

    WebIDL::ExceptionOr<GC::Ref<DOM::DocumentFragment>> parse_fragment(StringView markup);

    WebIDL::ExceptionOr<String> inner_html() const;
    WebIDL::ExceptionOr<void> set_inner_html(StringView);

    WebIDL::ExceptionOr<void> set_html_unsafe(StringView);

    WebIDL::ExceptionOr<String> get_html(GetHTMLOptions const&) const;

    WebIDL::ExceptionOr<void> insert_adjacent_html(String const& position, String const&);

    WebIDL::ExceptionOr<String> outer_html() const;
    WebIDL::ExceptionOr<void> set_outer_html(String const&);

    bool is_focused() const;
    bool is_active() const;
    bool is_target() const;
    bool is_document_element() const;

    bool is_shadow_host() const;
    GC::Ptr<ShadowRoot> shadow_root() { return m_shadow_root; }
    GC::Ptr<ShadowRoot const> shadow_root() const { return m_shadow_root; }
    void set_shadow_root(GC::Ptr<ShadowRoot>);

    void set_custom_properties(Optional<CSS::PseudoElement>, HashMap<FlyString, CSS::StyleProperty> custom_properties);
    [[nodiscard]] HashMap<FlyString, CSS::StyleProperty> const& custom_properties(Optional<CSS::PseudoElement>) const;

    bool style_uses_css_custom_properties() const { return m_style_uses_css_custom_properties; }
    void set_style_uses_css_custom_properties(bool value) { m_style_uses_css_custom_properties = value; }

    // NOTE: The function is wrapped in a GC::HeapFunction immediately.
    HTML::TaskID queue_an_element_task(HTML::Task::Source, Function<void()>);

    bool is_void_element() const;
    bool serializes_as_void() const;

    [[nodiscard]] CSSPixelRect get_bounding_client_rect() const;
    [[nodiscard]] GC::Ref<Geometry::DOMRect> get_bounding_client_rect_for_bindings() const;

    [[nodiscard]] Vector<CSSPixelRect> get_client_rects() const;
    [[nodiscard]] GC::Ref<Geometry::DOMRectList> get_client_rects_for_bindings() const;

    virtual GC::Ptr<Layout::Node> create_layout_node(GC::Ref<CSS::ComputedProperties>);
    virtual void adjust_computed_style(CSS::ComputedProperties&) { }

    virtual void did_receive_focus() { }
    virtual void did_lose_focus() { }

    static GC::Ptr<Layout::NodeWithStyle> create_layout_node_for_display_type(DOM::Document&, CSS::Display const&, GC::Ref<CSS::ComputedProperties>, Element*);

    bool affected_by_hover() const;
    bool includes_properties_from_invalidation_set(CSS::InvalidationSet const&) const;

    void set_pseudo_element_node(Badge<Layout::TreeBuilder>, CSS::PseudoElement, GC::Ptr<Layout::NodeWithStyle>);
    GC::Ptr<Layout::NodeWithStyle> get_pseudo_element_node(CSS::PseudoElement) const;
    bool has_pseudo_element(CSS::PseudoElement) const;
    bool has_pseudo_elements() const;
    void clear_pseudo_element_nodes(Badge<Layout::TreeBuilder>);
    void serialize_pseudo_elements_as_json(JsonArraySerializer<StringBuilder>& children_array) const;

    i32 tab_index() const;
    void set_tab_index(i32 tab_index);

    bool is_potentially_scrollable() const;

    double scroll_top() const;
    double scroll_left() const;
    void set_scroll_top(double y);
    void set_scroll_left(double x);

    int scroll_width() const;
    int scroll_height() const;

    bool is_actually_disabled() const;

    WebIDL::ExceptionOr<GC::Ptr<Element>> insert_adjacent_element(String const& where, GC::Ref<Element> element);
    WebIDL::ExceptionOr<void> insert_adjacent_text(String const& where, String const& data);

    // https://w3c.github.io/csswg-drafts/cssom-view-1/#dom-element-scrollintoview
    ErrorOr<void> scroll_into_view(Optional<Variant<bool, ScrollIntoViewOptions>> = {});

    // https://www.w3.org/TR/wai-aria-1.2/#ARIAMixin
#define __ENUMERATE_ARIA_ATTRIBUTE(name, attribute)                              \
    Optional<String> name() const override                                       \
    {                                                                            \
        return get_attribute(ARIA::AttributeNames::name);                        \
    }                                                                            \
                                                                                 \
    WebIDL::ExceptionOr<void> set_##name(Optional<String> const& value) override \
    {                                                                            \
        if (value.has_value())                                                   \
            TRY(set_attribute(ARIA::AttributeNames::name, *value));              \
        else                                                                     \
            remove_attribute(ARIA::AttributeNames::name);                        \
        return {};                                                               \
    }
    ENUMERATE_ARIA_ATTRIBUTES
#undef __ENUMERATE_ARIA_ATTRIBUTE

    GC::Ptr<DOM::Element> aria_active_descendant_element() { return m_aria_active_descendant_element; }
    void set_aria_active_descendant_element(GC::Ptr<DOM::Element> value) { m_aria_active_descendant_element = value; }

    virtual bool exclude_from_accessibility_tree() const override;

    virtual bool include_in_accessibility_tree() const override;

    virtual Element const* to_element() const override { return this; }

    bool is_hidden() const;
    bool has_hidden_ancestor() const;

    bool is_referenced() const;
    bool has_referenced_and_hidden_ancestor() const;

    void enqueue_a_custom_element_upgrade_reaction(HTML::CustomElementDefinition& custom_element_definition);
    void enqueue_a_custom_element_callback_reaction(FlyString const& callback_name, GC::RootVector<JS::Value> arguments);

    using CustomElementReactionQueue = Vector<Variant<CustomElementUpgradeReaction, CustomElementCallbackReaction>>;
    CustomElementReactionQueue* custom_element_reaction_queue() { return m_custom_element_reaction_queue; }
    CustomElementReactionQueue const* custom_element_reaction_queue() const { return m_custom_element_reaction_queue; }
    CustomElementReactionQueue& ensure_custom_element_reaction_queue();

    JS::ThrowCompletionOr<void> upgrade_element(GC::Ref<HTML::CustomElementDefinition> custom_element_definition);
    void try_to_upgrade();

    bool is_defined() const;
    bool is_custom() const;

    Optional<String> const& is_value() const { return m_is_value; }
    void set_is_value(Optional<String> const& is) { m_is_value = is; }

    void set_custom_element_state(CustomElementState);
    void setup_custom_element_from_constructor(HTML::CustomElementDefinition& custom_element_definition, Optional<String> const& is_value);

    void scroll(HTML::ScrollToOptions);
    void scroll(double x, double y);
    void scroll_by(HTML::ScrollToOptions);
    void scroll_by(double x, double y);

    bool check_visibility(Optional<CheckVisibilityOptions>);

    void register_intersection_observer(Badge<IntersectionObserver::IntersectionObserver>, IntersectionObserver::IntersectionObserverRegistration);
    void unregister_intersection_observer(Badge<IntersectionObserver::IntersectionObserver>, GC::Ref<IntersectionObserver::IntersectionObserver>);
    IntersectionObserver::IntersectionObserverRegistration& get_intersection_observer_registration(Badge<DOM::Document>, IntersectionObserver::IntersectionObserver const&);

    enum class ScrollOffsetFor {
        Self,
        PseudoBefore,
        PseudoAfter
    };
    CSSPixelPoint scroll_offset(ScrollOffsetFor type) const { return m_scroll_offset[to_underlying(type)]; }
    void set_scroll_offset(ScrollOffsetFor type, CSSPixelPoint offset) { m_scroll_offset[to_underlying(type)] = offset; }

    enum class Dir {
        Ltr,
        Rtl,
        Auto,
    };
    Optional<Dir> dir() const { return m_dir; }

    enum class Directionality {
        Ltr,
        Rtl,
    };
    Directionality directionality() const;

    Optional<FlyString> const& id() const { return m_id; }
    Optional<FlyString> const& name() const { return m_name; }

    virtual GC::Ptr<GC::Function<void()>> take_lazy_load_resumption_steps(Badge<DOM::Document>)
    {
        return nullptr;
    }

    // An element el is in the top layer if el is contained in its node document’s top layer
    // but not contained in its node document’s pending top layer removals.
    void set_in_top_layer(bool in_top_layer) { m_in_top_layer = in_top_layer; }
    bool in_top_layer() const { return m_in_top_layer; }

    // An element el is rendered in the top layer if el is contained in its node document’s top layer,
    // FIXME: and el has overlay: auto.
    void set_rendered_in_top_layer(bool rendered_in_top_layer) { m_rendered_in_top_layer = rendered_in_top_layer; }
    bool rendered_in_top_layer() const { return m_rendered_in_top_layer; }

    bool has_non_empty_counters_set() const { return m_counters_set; }
    Optional<CSS::CountersSet const&> counters_set();
    CSS::CountersSet& ensure_counters_set();
    void resolve_counters(CSS::ComputedProperties&);
    void inherit_counters();

    ProximityToTheViewport proximity_to_the_viewport() const { return m_proximity_to_the_viewport; }
    void determine_proximity_to_the_viewport();
    bool is_relevant_to_the_user();

    // https://drafts.csswg.org/css-contain-2/#skips-its-contents
    bool skips_its_contents();

    // https://drafts.csswg.org/css-contain-2/#containment-types
    bool has_size_containment() const;
    bool has_inline_size_containment() const;
    bool has_layout_containment() const;
    bool has_style_containment() const;
    bool has_paint_containment() const;

    bool matches_enabled_pseudo_class() const;
    bool matches_disabled_pseudo_class() const;
    bool matches_checked_pseudo_class() const;
    bool matches_placeholder_shown_pseudo_class() const;
    bool matches_link_pseudo_class() const;
    bool matches_local_link_pseudo_class() const;

    void invalidate_style_if_affected_by_has();

    bool affected_by_has_pseudo_class_in_subject_position() const { return m_affected_by_has_pseudo_class_in_subject_position; }
    void set_affected_by_has_pseudo_class_in_subject_position(bool value) { m_affected_by_has_pseudo_class_in_subject_position = value; }

    bool affected_by_has_pseudo_class_in_non_subject_position() const { return m_affected_by_has_pseudo_class_in_non_subject_position; }
    void set_affected_by_has_pseudo_class_in_non_subject_position(bool value) { m_affected_by_has_pseudo_class_in_non_subject_position = value; }

    bool affected_by_has_pseudo_class_with_relative_selector_that_has_sibling_combinator() const { return m_affected_by_has_pseudo_class_with_relative_selector_that_has_sibling_combinator; }
    void set_affected_by_has_pseudo_class_with_relative_selector_that_has_sibling_combinator(bool value) { m_affected_by_has_pseudo_class_with_relative_selector_that_has_sibling_combinator = value; }

    bool affected_by_direct_sibling_combinator() const { return m_affected_by_direct_sibling_combinator; }
    void set_affected_by_direct_sibling_combinator(bool value) { m_affected_by_direct_sibling_combinator = value; }

    bool affected_by_indirect_sibling_combinator() const { return m_affected_by_indirect_sibling_combinator; }
    void set_affected_by_indirect_sibling_combinator(bool value) { m_affected_by_indirect_sibling_combinator = value; }

    bool affected_by_first_or_last_child_pseudo_class() const { return m_affected_by_first_or_last_child_pseudo_class; }
    void set_affected_by_first_or_last_child_pseudo_class(bool value) { m_affected_by_first_or_last_child_pseudo_class = value; }

    bool affected_by_nth_child_pseudo_class() const { return m_affected_by_nth_child_pseudo_class; }
    void set_affected_by_nth_child_pseudo_class(bool value) { m_affected_by_nth_child_pseudo_class = value; }

    size_t sibling_invalidation_distance() const { return m_sibling_invalidation_distance; }
    void set_sibling_invalidation_distance(size_t value) { m_sibling_invalidation_distance = value; }

    bool style_affected_by_structural_changes() const
    {
        return affected_by_direct_sibling_combinator() || affected_by_indirect_sibling_combinator() || affected_by_first_or_last_child_pseudo_class() || affected_by_nth_child_pseudo_class();
    }

    size_t number_of_owned_list_items() const;
    Element const* list_owner() const;
    size_t ordinal_value() const;

protected:
    Element(Document&, DOM::QualifiedName);
    virtual void initialize(JS::Realm&) override;

    virtual void inserted() override;
    virtual void removed_from(Node* old_parent, Node& old_root) override;
    virtual void children_changed(ChildrenChangedMetadata const*) override;
    virtual i32 default_tab_index_value() const;

    // https://dom.spec.whatwg.org/#concept-element-attributes-change-ext
    virtual void attribute_changed(FlyString const& local_name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_);

    virtual void computed_properties_changed() { }

    virtual void visit_edges(Cell::Visitor&) override;

    virtual bool id_reference_exists(String const&) const override;

    CustomElementState custom_element_state() const { return m_custom_element_state; }

private:
    void make_html_uppercased_qualified_name();

    void invalidate_style_after_attribute_change(FlyString const& attribute_name, Optional<String> const& old_value, Optional<String> const& new_value);

    WebIDL::ExceptionOr<GC::Ptr<Node>> insert_adjacent(StringView where, GC::Ref<Node> node);

    void enqueue_an_element_on_the_appropriate_element_queue();

    Optional<Directionality> auto_directionality() const;
    Optional<Directionality> contained_text_auto_directionality(bool can_exclude_root) const;
    Directionality parent_directionality() const;
    bool is_auto_directionality_form_associated_element() const;

    QualifiedName m_qualified_name;
    FlyString m_html_uppercased_qualified_name;

    GC::Ptr<NamedNodeMap> m_attributes;
    GC::Ptr<CSS::CSSStyleProperties> m_inline_style;
    GC::Ptr<DOMTokenList> m_class_list;
    GC::Ptr<ShadowRoot> m_shadow_root;

    GC::Ptr<CSS::CascadedProperties> m_cascaded_properties;
    GC::Ptr<CSS::ComputedProperties> m_computed_properties;
    HashMap<FlyString, CSS::StyleProperty> m_custom_properties;

    struct PseudoElement {
        GC::Ptr<Layout::NodeWithStyle> layout_node;
        GC::Ptr<CSS::CascadedProperties> cascaded_properties;
        GC::Ptr<CSS::ComputedProperties> computed_properties;
        HashMap<FlyString, CSS::StyleProperty> custom_properties;
    };
    // TODO: CSS::Selector::PseudoElement includes a lot of pseudo-elements that exist in shadow trees,
    //       and so we don't want to include data for them here.
    using PseudoElementData = Array<PseudoElement, to_underlying(CSS::PseudoElement::KnownPseudoElementCount)>;
    mutable OwnPtr<PseudoElementData> m_pseudo_element_data;
    Optional<PseudoElement&> get_pseudo_element(CSS::PseudoElement) const;
    PseudoElement& ensure_pseudo_element(CSS::PseudoElement) const;

    Optional<CSS::PseudoElement> m_use_pseudo_element;

    Vector<FlyString> m_classes;
    Optional<Dir> m_dir;

    Optional<FlyString> m_id;
    Optional<FlyString> m_name;

    // https://html.spec.whatwg.org/multipage/custom-elements.html#custom-element-reaction-queue
    // All elements have an associated custom element reaction queue, initially empty. Each item in the custom element reaction queue is of one of two types:
    // NOTE: See the structs at the top of this header.
    OwnPtr<CustomElementReactionQueue> m_custom_element_reaction_queue;

    // https://dom.spec.whatwg.org/#concept-element-custom-element-state
    CustomElementState m_custom_element_state { CustomElementState::Undefined };

    // https://dom.spec.whatwg.org/#concept-element-custom-element-definition
    GC::Ptr<HTML::CustomElementDefinition> m_custom_element_definition;

    // https://dom.spec.whatwg.org/#concept-element-is-value
    Optional<String> m_is_value;

    // https://www.w3.org/TR/intersection-observer/#dom-element-registeredintersectionobservers-slot
    // Element objects have an internal [[RegisteredIntersectionObservers]] slot, which is initialized to an empty list.
    OwnPtr<Vector<IntersectionObserver::IntersectionObserverRegistration>> m_registered_intersection_observers;

    Array<CSSPixelPoint, 3> m_scroll_offset;

    bool m_in_top_layer : 1 { false };
    bool m_rendered_in_top_layer : 1 { false };
    bool m_style_uses_css_custom_properties { false };
    bool m_affected_by_has_pseudo_class_in_subject_position : 1 { false };
    bool m_affected_by_has_pseudo_class_in_non_subject_position : 1 { false };
    bool m_affected_by_direct_sibling_combinator : 1 { false };
    bool m_affected_by_indirect_sibling_combinator : 1 { false };
    bool m_affected_by_first_or_last_child_pseudo_class : 1 { false };
    bool m_affected_by_nth_child_pseudo_class : 1 { false };
    bool m_affected_by_has_pseudo_class_with_relative_selector_that_has_sibling_combinator : 1 { false };

    size_t m_sibling_invalidation_distance { 0 };

    OwnPtr<CSS::CountersSet> m_counters_set;

    GC::Ptr<DOM::Element> m_aria_active_descendant_element;

    // https://drafts.csswg.org/css-contain/#proximity-to-the-viewport
    ProximityToTheViewport m_proximity_to_the_viewport { ProximityToTheViewport::NotDetermined };
};

template<>
inline bool Node::fast_is<Element>() const { return is_element(); }

inline Element* Node::parent_element()
{
    auto* parent = this->parent();
    if (!parent || !is<Element>(parent))
        return nullptr;
    return static_cast<Element*>(parent);
}

inline Element const* Node::parent_element() const
{
    auto const* parent = this->parent();
    if (!parent || !is<Element>(parent))
        return nullptr;
    return static_cast<Element const*>(parent);
}

inline bool Element::has_class(FlyString const& class_name, CaseSensitivity case_sensitivity) const
{
    if (case_sensitivity == CaseSensitivity::CaseSensitive) {
        return any_of(m_classes, [&](auto& it) {
            return it == class_name;
        });
    }
    return any_of(m_classes, [&](auto& it) {
        return it.equals_ignoring_ascii_case(class_name);
    });
}

inline bool Element::has_pseudo_element(CSS::PseudoElement type) const
{
    if (!m_pseudo_element_data)
        return false;
    if (!CSS::Selector::PseudoElementSelector::is_known_pseudo_element_type(type))
        return false;
    return m_pseudo_element_data->at(to_underlying(type)).layout_node;
}

WebIDL::ExceptionOr<QualifiedName> validate_and_extract(JS::Realm&, Optional<FlyString> namespace_, FlyString const& qualified_name);

}
