/*
 * Copyright (c) 2018-2024, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ARIA/ARIAMixin.h>
#include <LibWeb/Animations/Animatable.h>
#include <LibWeb/Bindings/ElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/ShadowRootPrototype.h>
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
    Vector<JS::Handle<ShadowRoot>> shadow_roots {};
};

// https://w3c.github.io/csswg-drafts/cssom-view-1/#dictdef-scrollintoviewoptions
struct ScrollIntoViewOptions : public HTML::ScrollOptions {
    Bindings::ScrollLogicalPosition block { Bindings::ScrollLogicalPosition::Start };
    Bindings::ScrollLogicalPosition inline_ { Bindings::ScrollLogicalPosition::Nearest };
};

// https://html.spec.whatwg.org/multipage/custom-elements.html#upgrade-reaction
// An upgrade reaction, which will upgrade the custom element and contains a custom element definition; or
struct CustomElementUpgradeReaction {
    JS::Handle<HTML::CustomElementDefinition> custom_element_definition;
};

// https://html.spec.whatwg.org/multipage/custom-elements.html#callback-reaction
// A callback reaction, which will call a lifecycle callback, and contains a callback function as well as a list of arguments.
struct CustomElementCallbackReaction {
    JS::Handle<WebIDL::CallbackType> callback;
    JS::MarkedVector<JS::Value> arguments;
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

    // NOTE: This is for the JS bindings
    Optional<FlyString> const& namespace_uri() const { return m_qualified_name.namespace_(); }

    bool has_attribute(FlyString const& name) const;
    bool has_attribute_ns(Optional<FlyString> const& namespace_, FlyString const& name) const;
    bool has_attributes() const;

    Optional<String> attribute(FlyString const& name) const { return get_attribute(name); }

    Optional<String> get_attribute(FlyString const& name) const;
    Optional<String> get_attribute_ns(Optional<FlyString> const& namespace_, FlyString const& name) const;
    String get_attribute_value(FlyString const& local_name, Optional<FlyString> const& namespace_ = {}) const;

    WebIDL::ExceptionOr<void> set_attribute(FlyString const& name, String const& value);

    WebIDL::ExceptionOr<void> set_attribute_ns(Optional<FlyString> const& namespace_, FlyString const& qualified_name, String const& value);
    void set_attribute_value(FlyString const& local_name, String const& value, Optional<FlyString> const& prefix = {}, Optional<FlyString> const& namespace_ = {});
    WebIDL::ExceptionOr<JS::GCPtr<Attr>> set_attribute_node(Attr&);
    WebIDL::ExceptionOr<JS::GCPtr<Attr>> set_attribute_node_ns(Attr&);

    void append_attribute(FlyString const& name, String const& value);
    void append_attribute(Attr&);
    void remove_attribute(FlyString const& name);
    void remove_attribute_ns(Optional<FlyString> const& namespace_, FlyString const& name);
    WebIDL::ExceptionOr<JS::NonnullGCPtr<Attr>> remove_attribute_node(JS::NonnullGCPtr<Attr>);

    WebIDL::ExceptionOr<bool> toggle_attribute(FlyString const& name, Optional<bool> force);
    size_t attribute_list_size() const;
    NamedNodeMap const* attributes() const { return m_attributes.ptr(); }
    Vector<String> get_attribute_names() const;

    JS::GCPtr<Attr> get_attribute_node(FlyString const& name) const;
    JS::GCPtr<Attr> get_attribute_node_ns(Optional<FlyString> const& namespace_, FlyString const& name) const;

    DOMTokenList* class_list();

    WebIDL::ExceptionOr<JS::NonnullGCPtr<ShadowRoot>> attach_shadow(ShadowRootInit init);
    WebIDL::ExceptionOr<void> attach_a_shadow_root(Bindings::ShadowRootMode mode, bool clonable, bool serializable, bool delegates_focus, Bindings::SlotAssignmentMode slot_assignment);
    JS::GCPtr<ShadowRoot> shadow_root_for_bindings() const;

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

    virtual void apply_presentational_hints(CSS::StyleProperties&) const { }

    // https://dom.spec.whatwg.org/#concept-element-attributes-change-ext
    virtual void attribute_change_steps(FlyString const& local_name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_);

    void run_attribute_change_steps(FlyString const& local_name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_);
    virtual void attribute_changed(FlyString const& name, Optional<String> const& value);

    CSS::RequiredInvalidationAfterStyleChange recompute_style();

    Optional<CSS::Selector::PseudoElement::Type> use_pseudo_element() const { return m_use_pseudo_element; }
    void set_use_pseudo_element(Optional<CSS::Selector::PseudoElement::Type> use_pseudo_element) { m_use_pseudo_element = move(use_pseudo_element); }

    JS::GCPtr<Layout::NodeWithStyle> layout_node();
    JS::GCPtr<Layout::NodeWithStyle const> layout_node() const;

    CSS::StyleProperties* computed_css_values() { return m_computed_css_values.ptr(); }
    CSS::StyleProperties const* computed_css_values() const { return m_computed_css_values.ptr(); }
    void set_computed_css_values(RefPtr<CSS::StyleProperties>);
    NonnullRefPtr<CSS::StyleProperties> resolved_css_values();

    void reset_animated_css_properties();

    JS::GCPtr<CSS::ElementInlineCSSStyleDeclaration const> inline_style() const { return m_inline_style; }

    CSS::CSSStyleDeclaration* style_for_bindings();

    CSS::StyleSheetList& document_or_shadow_root_style_sheets();

    WebIDL::ExceptionOr<JS::NonnullGCPtr<DOM::DocumentFragment>> parse_fragment(StringView markup);

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

    JS::NonnullGCPtr<HTMLCollection> get_elements_by_class_name(StringView);

    bool is_shadow_host() const;
    JS::GCPtr<ShadowRoot> shadow_root() { return m_shadow_root; }
    JS::GCPtr<ShadowRoot const> shadow_root() const { return m_shadow_root; }
    void set_shadow_root(JS::GCPtr<ShadowRoot>);

    void set_custom_properties(Optional<CSS::Selector::PseudoElement::Type>, HashMap<FlyString, CSS::StyleProperty> custom_properties);
    [[nodiscard]] HashMap<FlyString, CSS::StyleProperty> const& custom_properties(Optional<CSS::Selector::PseudoElement::Type>) const;

    // NOTE: The function is wrapped in a JS::HeapFunction immediately.
    int queue_an_element_task(HTML::Task::Source, Function<void()>);

    bool is_void_element() const;
    bool serializes_as_void() const;

    JS::NonnullGCPtr<Geometry::DOMRect> get_bounding_client_rect() const;
    JS::NonnullGCPtr<Geometry::DOMRectList> get_client_rects() const;

    virtual JS::GCPtr<Layout::Node> create_layout_node(NonnullRefPtr<CSS::StyleProperties>);
    virtual void adjust_computed_style(CSS::StyleProperties&) { }

    virtual void did_receive_focus() { }
    virtual void did_lose_focus() { }

    static JS::GCPtr<Layout::Node> create_layout_node_for_display_type(DOM::Document&, CSS::Display const&, NonnullRefPtr<CSS::StyleProperties>, Element*);

    void set_pseudo_element_node(Badge<Layout::TreeBuilder>, CSS::Selector::PseudoElement::Type, JS::GCPtr<Layout::Node>);
    JS::GCPtr<Layout::Node> get_pseudo_element_node(CSS::Selector::PseudoElement::Type) const;
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

    WebIDL::ExceptionOr<JS::GCPtr<Element>> insert_adjacent_element(String const& where, JS::NonnullGCPtr<Element> element);
    WebIDL::ExceptionOr<void> insert_adjacent_text(String const& where, String const& data);

    // https://w3c.github.io/csswg-drafts/cssom-view-1/#dom-element-scrollintoview
    ErrorOr<void> scroll_into_view(Optional<Variant<bool, ScrollIntoViewOptions>> = {});

    // https://www.w3.org/TR/wai-aria-1.2/#ARIAMixin
#define ARIA_IMPL(name, attribute)                                               \
    Optional<String> name() const override                                       \
    {                                                                            \
        return get_attribute(attribute);                                         \
    }                                                                            \
                                                                                 \
    WebIDL::ExceptionOr<void> set_##name(Optional<String> const& value) override \
    {                                                                            \
        if (value.has_value())                                                   \
            TRY(set_attribute(attribute, *value));                               \
        else                                                                     \
            remove_attribute(attribute);                                         \
        return {};                                                               \
    }

    // https://www.w3.org/TR/wai-aria-1.2/#accessibilityroleandproperties-correspondence
    ARIA_IMPL(role, "role"_fly_string);
    ARIA_IMPL(aria_active_descendant, "aria-activedescendant"_fly_string);
    ARIA_IMPL(aria_atomic, "aria-atomic"_fly_string);
    ARIA_IMPL(aria_auto_complete, "aria-autocomplete"_fly_string);
    ARIA_IMPL(aria_busy, "aria-busy"_fly_string);
    ARIA_IMPL(aria_checked, "aria-checked"_fly_string);
    ARIA_IMPL(aria_col_count, "aria-colcount"_fly_string);
    ARIA_IMPL(aria_col_index, "aria-colindex"_fly_string);
    ARIA_IMPL(aria_col_span, "aria-colspan"_fly_string);
    ARIA_IMPL(aria_controls, "aria-controls"_fly_string);
    ARIA_IMPL(aria_current, "aria-current"_fly_string);
    ARIA_IMPL(aria_described_by, "aria-describedby"_fly_string);
    ARIA_IMPL(aria_details, "aria-details"_fly_string);
    ARIA_IMPL(aria_drop_effect, "aria-dropeffect"_fly_string);
    ARIA_IMPL(aria_error_message, "aria-errormessage"_fly_string);
    ARIA_IMPL(aria_disabled, "aria-disabled"_fly_string);
    ARIA_IMPL(aria_expanded, "aria-expanded"_fly_string);
    ARIA_IMPL(aria_flow_to, "aria-flowto"_fly_string);
    ARIA_IMPL(aria_grabbed, "aria-grabbed"_fly_string);
    ARIA_IMPL(aria_has_popup, "aria-haspopup"_fly_string);
    ARIA_IMPL(aria_hidden, "aria-hidden"_fly_string);
    ARIA_IMPL(aria_invalid, "aria-invalid"_fly_string);
    ARIA_IMPL(aria_key_shortcuts, "aria-keyshortcuts"_fly_string);
    ARIA_IMPL(aria_label, "aria-label"_fly_string);
    ARIA_IMPL(aria_labelled_by, "aria-labelledby"_fly_string);
    ARIA_IMPL(aria_level, "aria-level"_fly_string);
    ARIA_IMPL(aria_live, "aria-live"_fly_string);
    ARIA_IMPL(aria_modal, "aria-modal"_fly_string);
    ARIA_IMPL(aria_multi_line, "aria-multiline"_fly_string);
    ARIA_IMPL(aria_multi_selectable, "aria-multiselectable"_fly_string);
    ARIA_IMPL(aria_orientation, "aria-orientation"_fly_string);
    ARIA_IMPL(aria_owns, "aria-owns"_fly_string);
    ARIA_IMPL(aria_placeholder, "aria-placeholder"_fly_string);
    ARIA_IMPL(aria_pos_in_set, "aria-posinset"_fly_string);
    ARIA_IMPL(aria_pressed, "aria-pressed"_fly_string);
    ARIA_IMPL(aria_read_only, "aria-readonly"_fly_string);
    ARIA_IMPL(aria_relevant, "aria-relevant"_fly_string);
    ARIA_IMPL(aria_required, "aria-required"_fly_string);
    ARIA_IMPL(aria_role_description, "aria-roledescription"_fly_string);
    ARIA_IMPL(aria_row_count, "aria-rowcount"_fly_string);
    ARIA_IMPL(aria_row_index, "aria-rowindex"_fly_string);
    ARIA_IMPL(aria_row_span, "aria-rowspan"_fly_string);
    ARIA_IMPL(aria_selected, "aria-selected"_fly_string);
    ARIA_IMPL(aria_set_size, "aria-setsize"_fly_string);
    ARIA_IMPL(aria_sort, "aria-sort"_fly_string);
    ARIA_IMPL(aria_value_max, "aria-valuemax"_fly_string);
    ARIA_IMPL(aria_value_min, "aria-valuemin"_fly_string);
    ARIA_IMPL(aria_value_now, "aria-valuenow"_fly_string);
    ARIA_IMPL(aria_value_text, "aria-valuetext"_fly_string);

#undef ARIA_IMPL

    virtual bool exclude_from_accessibility_tree() const override;

    virtual bool include_in_accessibility_tree() const override;

    void enqueue_a_custom_element_upgrade_reaction(HTML::CustomElementDefinition& custom_element_definition);
    void enqueue_a_custom_element_callback_reaction(FlyString const& callback_name, JS::MarkedVector<JS::Value> arguments);

    using CustomElementReactionQueue = Vector<Variant<CustomElementUpgradeReaction, CustomElementCallbackReaction>>;
    CustomElementReactionQueue* custom_element_reaction_queue() { return m_custom_element_reaction_queue; }
    CustomElementReactionQueue const* custom_element_reaction_queue() const { return m_custom_element_reaction_queue; }
    CustomElementReactionQueue& ensure_custom_element_reaction_queue();

    JS::ThrowCompletionOr<void> upgrade_element(JS::NonnullGCPtr<HTML::CustomElementDefinition> custom_element_definition);
    void try_to_upgrade();

    bool is_defined() const;
    bool is_custom() const;

    Optional<String> const& is_value() const { return m_is_value; }
    void set_is_value(Optional<String> const& is) { m_is_value = is; }

    void set_custom_element_state(CustomElementState value) { m_custom_element_state = value; }
    void setup_custom_element_from_constructor(HTML::CustomElementDefinition& custom_element_definition, Optional<String> const& is_value);

    void scroll(HTML::ScrollToOptions);
    void scroll(double x, double y);
    void scroll_by(HTML::ScrollToOptions);
    void scroll_by(double x, double y);

    void register_intersection_observer(Badge<IntersectionObserver::IntersectionObserver>, IntersectionObserver::IntersectionObserverRegistration);
    void unregister_intersection_observer(Badge<IntersectionObserver::IntersectionObserver>, JS::NonnullGCPtr<IntersectionObserver::IntersectionObserver>);
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

    virtual JS::GCPtr<JS::HeapFunction<void()>> take_lazy_load_resumption_steps(Badge<DOM::Document>)
    {
        return nullptr;
    }

    void set_in_top_layer(bool in_top_layer) { m_in_top_layer = in_top_layer; }
    bool in_top_layer() const { return m_in_top_layer; }

protected:
    Element(Document&, DOM::QualifiedName);
    virtual void initialize(JS::Realm&) override;

    virtual void inserted() override;
    virtual void removed_from(Node*) override;
    virtual void children_changed() override;
    virtual i32 default_tab_index_value() const;

    virtual void computed_css_values_changed() { }

    virtual void visit_edges(Cell::Visitor&) override;

    virtual bool id_reference_exists(String const&) const override;

    CustomElementState custom_element_state() const { return m_custom_element_state; }

private:
    void make_html_uppercased_qualified_name();

    void invalidate_style_after_attribute_change(FlyString const& attribute_name);

    WebIDL::ExceptionOr<JS::GCPtr<Node>> insert_adjacent(StringView where, JS::NonnullGCPtr<Node> node);

    void enqueue_an_element_on_the_appropriate_element_queue();

    Optional<Directionality> auto_directionality() const;
    Directionality parent_directionality() const;
    bool is_auto_directionality_form_associated_element() const;

    QualifiedName m_qualified_name;
    FlyString m_html_uppercased_qualified_name;

    JS::GCPtr<NamedNodeMap> m_attributes;
    JS::GCPtr<CSS::ElementInlineCSSStyleDeclaration> m_inline_style;
    JS::GCPtr<DOMTokenList> m_class_list;
    JS::GCPtr<ShadowRoot> m_shadow_root;

    RefPtr<CSS::StyleProperties> m_computed_css_values;
    HashMap<FlyString, CSS::StyleProperty> m_custom_properties;

    using PseudoElementCustomProperties = Array<HashMap<FlyString, CSS::StyleProperty>, to_underlying(CSS::Selector::PseudoElement::Type::KnownPseudoElementCount)>;
    mutable OwnPtr<PseudoElementCustomProperties> m_pseudo_element_custom_properties;
    PseudoElementCustomProperties& pseudo_element_custom_properties() const;

    Optional<CSS::Selector::PseudoElement::Type> m_use_pseudo_element;

    Vector<FlyString> m_classes;
    Optional<Dir> m_dir;

    Optional<FlyString> m_id;
    Optional<FlyString> m_name;

    using PseudoElementLayoutNodes = Array<JS::GCPtr<Layout::Node>, to_underlying(CSS::Selector::PseudoElement::Type::KnownPseudoElementCount)>;
    OwnPtr<PseudoElementLayoutNodes> m_pseudo_element_nodes;

    // https://html.spec.whatwg.org/multipage/custom-elements.html#custom-element-reaction-queue
    // All elements have an associated custom element reaction queue, initially empty. Each item in the custom element reaction queue is of one of two types:
    // NOTE: See the structs at the top of this header.
    OwnPtr<CustomElementReactionQueue> m_custom_element_reaction_queue;

    // https://dom.spec.whatwg.org/#concept-element-custom-element-state
    CustomElementState m_custom_element_state { CustomElementState::Undefined };

    // https://dom.spec.whatwg.org/#concept-element-custom-element-definition
    JS::GCPtr<HTML::CustomElementDefinition> m_custom_element_definition;

    // https://dom.spec.whatwg.org/#concept-element-is-value
    Optional<String> m_is_value;

    // https://www.w3.org/TR/intersection-observer/#dom-element-registeredintersectionobservers-slot
    // Element objects have an internal [[RegisteredIntersectionObservers]] slot, which is initialized to an empty list.
    OwnPtr<Vector<IntersectionObserver::IntersectionObserverRegistration>> m_registered_intersection_observers;

    Array<CSSPixelPoint, 3> m_scroll_offset;

    bool m_in_top_layer { false };
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

WebIDL::ExceptionOr<QualifiedName> validate_and_extract(JS::Realm&, Optional<FlyString> namespace_, FlyString const& qualified_name);

}
