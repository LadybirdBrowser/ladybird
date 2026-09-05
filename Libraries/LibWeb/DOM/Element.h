/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Badge.h>
#include <AK/Concepts.h>
#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <LibGC/RootVector.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/ARIA/ARIAMixin.h>
#include <LibWeb/Animations/Animatable.h>
#include <LibWeb/Bindings/Element.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/StyleInputRecord.h>
#include <LibWeb/CSS/StyleProperty.h>
#include <LibWeb/DOM/ChildNode.h>
#include <LibWeb/DOM/NonDocumentTypeChildNode.h>
#include <LibWeb/DOM/ParentNode.h>
#include <LibWeb/DOM/PseudoElement.h>
#include <LibWeb/DOM/QualifiedName.h>
#include <LibWeb/DOM/RequestFullscreenError.h>
#include <LibWeb/DOM/Slottable.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Fullscreen/FullscreenRequestType.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/EventLoop/Task.h>
#include <LibWeb/HTML/Parser/ParserScriptingMode.h>
#include <LibWeb/HTML/TagNames.h>
#include <LibWeb/HTML/TokenizedFeatures.h>
#include <LibWeb/HTML/UserNavigationInvolvement.h>
#include <LibWeb/TrustedTypes/TrustedHTML.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::Animations {

struct AnimationUpdateContext;
class KeyframeEffect;

}

namespace Web::CSS {

struct StyleEngineMatchResult;

}

namespace Web::DOM {

class Element;

}

namespace Web::HTML {

template<typename>
class HTMLOrSVGOrMathMLElement;

}

namespace Web::Bindings {

class PlatformObject;
class WrapperWorld;
enum class ScrollBehavior : u8;
enum class ScrollIntoViewContainer : u8;
enum class ScrollLogicalPosition : u8;
struct GetHTMLOptions;
struct PointerLockOptions;
WEB_API void set_prototype_from_custom_element_definition_if_needed(DOM::Element&, PlatformObject&);
WEB_API JS::Value element(JS::Realm&, GC::Ref<DOM::Element>);
WEB_API DOM::Element* element_from_value(JS::Value);
WEB_API GC::Ref<Geometry::DOMRect> get_bounding_client_rect(DOM::Element const&);
WEB_API GC::Ref<Geometry::DOMRectList> get_client_rects(DOM::Element const&);
WEB_API GC::Ptr<JS::Array> cached_reflected_element_array(DOM::Element&, WrapperWorld const&, FlyString const&);
WEB_API GC::Ptr<JS::Array> cached_reflected_element_array(DOM::Element&, WrapperWorld const&, Utf16FlyString const&);
WEB_API void set_cached_reflected_element_array(DOM::Element&, WrapperWorld const&, FlyString const&, GC::Ptr<JS::Array>);
WEB_API void set_cached_reflected_element_array(DOM::Element&, WrapperWorld const&, Utf16FlyString const&, GC::Ptr<JS::Array>);
WEB_API bool cached_reflected_element_array_contains_same_elements(GC::Ptr<JS::Array>, Optional<GC::RootVector<GC::Ref<DOM::Element>>> const&);
WEB_API GC::Ref<WebIDL::Promise> request_pointer_lock(DOM::Element&, Optional<PointerLockOptions> const&);
WEB_API WebIDL::ExceptionOr<TrustedTypes::TrustedHTMLOrString> inner_html(DOM::Element&);
WEB_API WebIDL::ExceptionOr<void> set_inner_html(DOM::Element&, TrustedTypes::TrustedHTMLOrString const&);
WEB_API WebIDL::ExceptionOr<TrustedTypes::TrustedHTMLOrString> outer_html(DOM::Element&);
WEB_API WebIDL::ExceptionOr<void> set_outer_html(DOM::Element&, TrustedTypes::TrustedHTMLOrString const&);
WEB_API WebIDL::ExceptionOr<void> set_html_unsafe(DOM::Element&, Variant<GC::Ref<TrustedTypes::TrustedHTML>, Utf16String> const&);
WEB_API WebIDL::ExceptionOr<void> insert_adjacent_html(DOM::Element&, Utf16String const& position, Variant<GC::Ref<TrustedTypes::TrustedHTML>, Utf16String> const&);
WEB_API WebIDL::ExceptionOr<void> set_attribute(DOM::Element&, Utf16String const&, Variant<GC::Ref<TrustedTypes::TrustedHTML>, GC::Ref<TrustedTypes::TrustedScript>, GC::Ref<TrustedTypes::TrustedScriptURL>, Utf16String> const&);
WEB_API WebIDL::ExceptionOr<void> set_attribute_ns(DOM::Element&, Optional<Utf16FlyString> const&, Utf16FlyString const&, Variant<GC::Ref<TrustedTypes::TrustedHTML>, GC::Ref<TrustedTypes::TrustedScript>, GC::Ref<TrustedTypes::TrustedScriptURL>, Utf16String> const&);

}

namespace Web::DOM {

// https://html.spec.whatwg.org/multipage/custom-elements.html#upgrade-reaction
// An upgrade reaction, which will upgrade the custom element and contains a custom element definition; or
struct CustomElementUpgradeReaction {
    GC::Ref<HTML::CustomElementDefinition> custom_element_definition;
};

struct CustomElementAdoptedCallbackReactionArguments {
    GC::Ref<Document> old_document;
    GC::Ref<Document> new_document;
};

struct CustomElementAttributeChangedCallbackReactionArguments {
    Utf16FlyString attribute_name;
    Optional<Utf16String> old_value;
    Optional<Utf16String> new_value;
    Optional<Utf16FlyString> namespace_uri;
};

struct CustomElementFormAssociatedCallbackReactionArguments {
    GC::Ptr<HTML::HTMLFormElement> form;
};

struct CustomElementFormDisabledCallbackReactionArguments {
    bool is_disabled { false };
};

using CustomElementCallbackReactionArguments = Variant<Empty, CustomElementAdoptedCallbackReactionArguments, CustomElementAttributeChangedCallbackReactionArguments, CustomElementFormAssociatedCallbackReactionArguments, CustomElementFormDisabledCallbackReactionArguments>;

// https://html.spec.whatwg.org/multipage/custom-elements.html#callback-reaction
// A callback reaction, which will call a lifecycle callback, and contains a callback function as well as a list of arguments.
struct CustomElementCallbackReaction {
    GC::Ref<WebIDL::CallbackType> callback;
    CustomElementCallbackReactionArguments arguments;
};

struct CustomElementConnectedMoveCallbackReaction {
    GC::Ptr<WebIDL::CallbackType> disconnected_callback;
    GC::Ptr<WebIDL::CallbackType> connected_callback;
};

// https://dom.spec.whatwg.org/#concept-element-custom-element-state
// An element’s custom element state is one of "undefined", "failed", "uncustomized", "precustomized", or "custom".
enum class CustomElementState : u8 {
    Undefined,
    Failed,
    Uncustomized,
    Precustomized,
    Custom,
};

// https://drafts.csswg.org/css-contain/#proximity-to-the-viewport
// An element that has content-visibility: auto is in one of three states when it comes to its proximity to the viewport:
enum class ProximityToTheViewport : u8 {
    // - The element is close to the viewport:
    CloseToTheViewport,
    // - The element is far away from the viewport:
    FarAwayFromTheViewport,
    // - The element’s proximity to the viewport is not determined:
    NotDetermined,
};

class WEB_API Element
    : public ParentNode
    , public ChildNode<Element>
    , public NonDocumentTypeChildNode<Element>
    , public SlottableMixin
    , public ARIA::ARIAMixin
    , public Animations::Animatable {
    WEB_WRAPPABLE(Element, ParentNode);
    GC_DECLARE_ALLOCATOR(Element);

public:
    virtual ~Element() override;

    virtual bool is_dom_element() const final { return true; }

    virtual Node& slottable_as_node() override { return *this; }

    Utf16FlyString const& qualified_name() const { return m_qualified_name.as_string(); }
    Utf16FlyString const& html_uppercased_qualified_name() const;

    virtual Utf16FlyString node_name() const final { return html_uppercased_qualified_name(); }
    Utf16FlyString const& local_name() const { return m_qualified_name.local_name(); }

    Utf16FlyString const& lowercased_local_name() const { return m_qualified_name.lowercased_local_name(); }

    // NOTE: This is for the JS bindings
    Utf16FlyString const& tag_name() const { return html_uppercased_qualified_name(); }

    Optional<Utf16FlyString> const& prefix() const { return m_qualified_name.prefix(); }

    void set_prefix(Optional<Utf16FlyString> value);

    Optional<Utf16String> locate_a_namespace_prefix(Optional<Utf16View> namespace_) const;

    // NOTE: This is for the JS bindings
    Optional<Utf16FlyString> const& namespace_uri() const { return m_qualified_name.namespace_(); }

    bool has_attribute(Utf16FlyString const& name) const;
    bool has_attribute_ns(Optional<Utf16FlyString> const& namespace_, Utf16FlyString const& name) const;
    bool has_attributes() const;

    Optional<Utf16String> attribute(Utf16FlyString const& name) const { return get_attribute(name); }

    Optional<Utf16String> get_attribute(Utf16FlyString const& name) const;
    Optional<Utf16String> get_attribute_ns(Optional<Utf16FlyString> const& namespace_, Utf16FlyString const& name) const;
    Utf16String get_attribute_value(Utf16FlyString const& local_name, Optional<Utf16FlyString> const& namespace_ = {}) const;

    Utf16String get_an_elements_target(Optional<Utf16String> target = {}) const;
    HTML::TokenizedFeature::NoOpener get_an_elements_noopener(URL::URL const& url, Utf16View target) const;

    bool cannot_navigate() const;

    HTML::HTMLHyperlinkElementUtils const* created_hyperlink() const;
    bool creates_a_hyperlink() const;
    void follow_the_hyperlink(Optional<Utf16String> hyperlink_suffix, HTML::UserNavigationInvolvement = HTML::UserNavigationInvolvement::None);
    void download_the_hyperlink(Optional<Utf16String> hyperlink_suffix, HTML::UserNavigationInvolvement = HTML::UserNavigationInvolvement::None);
    void activate_the_hyperlink(Event const&);

    Optional<Utf16String> lang() const;
    Optional<Utf16View> lang_view() const;
    void invalidate_lang_value();

    void set_attribute(FlyString qualified_name, Utf16String const& verified_value);
    void set_attribute_ns(QualifiedName const&, Utf16String const& verified_value);
    void set_attribute_value(FlyString const& local_name, String const& value, Optional<FlyString> const& prefix = {}, Optional<FlyString> const& namespace_ = {});
    void set_attribute_value(Utf16FlyString const& local_name, Utf16View value, Optional<Utf16FlyString> const& prefix = {}, Optional<Utf16FlyString> const& namespace_ = {});
    void set_attribute_value(Utf16FlyString const& local_name, Utf16String value, Optional<Utf16FlyString> const& prefix = {}, Optional<Utf16FlyString> const& namespace_ = {});
    WebIDL::ExceptionOr<GC::Ptr<Attr>> set_attribute_node(Attr&);
    WebIDL::ExceptionOr<GC::Ptr<Attr>> set_attribute_node_ns(Attr&);

    void append_attribute(Attr&);
    void append_attribute(QualifiedName, Utf16String value);
    void remove_attribute(Utf16FlyString const& name);
    void remove_attribute_ns(Optional<Utf16FlyString> const& namespace_, Utf16FlyString const& name);
    WebIDL::ExceptionOr<GC::Ref<Attr>> remove_attribute_node(GC::Ref<Attr>);

    WebIDL::ExceptionOr<bool> toggle_attribute(Utf16FlyString const& name, Optional<bool> force);
    size_t attribute_list_size() const;

    GC::Ptr<NamedNodeMap const> attributes() const;
    GC::Ptr<NamedNodeMap> attributes();

    Vector<Utf16FlyString> get_attribute_names() const;

    GC::Ptr<Attr> get_attribute_node(Utf16FlyString const& name) const;
    GC::Ptr<Attr> get_attribute_node_ns(Optional<Utf16FlyString> const& namespace_, Utf16FlyString const& name) const;

    GC::Ptr<DOM::Element> get_the_attribute_associated_element(Utf16FlyString const& content_attribute, GC::Ptr<DOM::Element> explicitly_set_attribute_element) const;
    Optional<GC::RootVector<GC::Ref<DOM::Element>>> get_the_attribute_associated_elements(Utf16FlyString const& content_attribute, Optional<Vector<GC::Weak<DOM::Element>> const&> explicitly_set_attribute_elements) const;

    GC::Ref<DOMTokenList> class_list();
    GC::Ref<DOMTokenList> part_list();
    ReadonlySpan<Utf16FlyString> part_names() const;

    using ShadowRootOptions = Bindings::ShadowRootInit;

    // https://drafts.csswg.org/css-shadow-1/#exportparts
    // The `exportparts` attribute as a list of (inner name, outer name) pairs.
    template<typename Callback>
    void for_each_exported_part(Callback callback) const
    {
        auto exportparts = get_attribute(HTML::AttributeNames::exportparts);
        if (!exportparts.has_value())
            return;

        exportparts->for_each_split_view(u',', SplitBehavior::Nothing, [&](Utf16View mapping) {
            auto trimmed = mapping.trim_ascii_whitespace();
            if (trimmed.is_empty())
                return IterationDecision::Continue;

            Vector<Utf16View, 2> parts;
            trimmed.for_each_split_view(u':', SplitBehavior::KeepEmpty, [&](Utf16View part) {
                parts.append(part);
                return IterationDecision::Continue;
            });
            if (parts.size() == 1) {
                auto name = parts[0].trim_ascii_whitespace();
                callback(name, name);
            } else if (parts.size() == 2) {
                callback(parts[0].trim_ascii_whitespace(), parts[1].trim_ascii_whitespace());
            }
            return IterationDecision::Continue;
        });
    }

    WebIDL::ExceptionOr<GC::Ref<ShadowRoot>> attach_shadow(ShadowRootOptions const&);
    WebIDL::ExceptionOr<void> attach_a_shadow_root(ShadowRootMode mode, bool clonable, bool serializable, bool delegates_focus, SlotAssignmentMode slot_assignment, GC::Ptr<HTML::CustomElementRegistry> registry);
    GC::Ptr<ShadowRoot> open_shadow_root() const;

    WebIDL::ExceptionOr<bool> matches(Utf16View selectors) const;
    WebIDL::ExceptionOr<DOM::Element const*> closest(Utf16View selectors) const;

    int client_top() const;
    int client_left() const;
    int client_width() const;
    int client_height() const;
    [[nodiscard]] double current_css_zoom() const;

    void for_each_attribute(Function<void(Attr&)>);
    void for_each_attribute(Function<void(Attr const&)>) const;

    void for_each_attribute(Function<void(QualifiedName, Utf16String)>) const;
    void for_each_attribute(Function<void(Utf16FlyString, Utf16String)>) const;
    void synchronize_all_attributes() const;

    bool has_class(Utf16View, CaseSensitivity = CaseSensitivity::CaseSensitive) const;
    bool has_class(Utf16FlyString const&, CaseSensitivity = CaseSensitivity::CaseSensitive) const;
    Vector<Utf16FlyString> const& class_names() const { return m_classes; }

    // The element's StyleEngine identity, or 0 while it has none. Disconnected and never-styled
    // elements keep 0, which is what makes them free.
    [[nodiscard]] CSS::StyleNodeID style_node_id() const { return m_style_node_id; }
    void set_style_node_id(CSS::StyleNodeID style_node_id)
    {
        if (m_style_node_id != style_node_id)
            m_published_presentational_hint_properties.clear();
        m_style_node_id = style_node_id;
    }

    // https://html.spec.whatwg.org/multipage/embedded-content-other.html#dimension-attributes
    virtual bool supports_dimension_attributes() const { return false; }

    virtual bool is_presentational_hint(Utf16FlyString const&) const { return false; }
    virtual void apply_presentational_hints(Vector<CSS::StyleProperty>&) const;
    bool presentational_hint_properties_need_publication(ReadonlySpan<CSS::StyleProperty>) const;
    void did_publish_presentational_hint_properties(ReadonlySpan<CSS::StyleProperty>);

    void run_attribute_change_steps(Utf16FlyString const& local_name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_);

    CSS::RequiredInvalidationAfterStyleChange apply_style_engine_reaction(bool& did_change_custom_properties);
    // Apply a base style record the style engine computed itself from this element's moved cascade
    // winners: no style computation runs here, only the diff against the old record and its effects.
    // The synthetic pseudo-element records the style engine settled beside an engine-computed record: a kind it
    // decided holds the record, or none when the pseudo-element is not generated; a kind it left alone is unchanged.
    using EnginePseudoElementRecords = Array<Optional<CSS::StyleRecordID>, to_underlying(CSS::PseudoElement::KnownPseudoElementCount)>;
    CSS::RequiredInvalidationAfterStyleChange apply_engine_computed_style_record(CSS::StyleRecordID new_style_record, EnginePseudoElementRecords const&, bool& did_change_custom_properties);
    CSS::RequiredInvalidationAfterStyleChange recompute_pseudo_element_styles_after_animation_update(Badge<Web::Animations::AnimationUpdateContext>);

    void set_needs_layout_tree_rebuild(SetNeedsLayoutTreeUpdateReason, CSS::LayoutTreeRebuildRoot);
    bool apply_box_presence_change_in_place(SetNeedsLayoutTreeUpdateReason);

    Optional<CSS::PseudoElement> associated_shadow_host_pseudo_element() const;
    void set_associated_shadow_host_pseudo_element(CSS::PseudoElement pseudo_element);

    Layout::NodeWithStyle* layout_node();
    Layout::NodeWithStyle const* layout_node() const;

    Layout::NodeWithStyle* unsafe_layout_node();
    Layout::NodeWithStyle const* unsafe_layout_node() const;

    [[nodiscard]] CSS::ComputedStyleRecordView computed_style(Optional<CSS::PseudoElement> = {}) const;
    [[nodiscard]] CSS::StyleRecordID style_record_identity(Optional<CSS::PseudoElement> = {}) const;
    u64 animation_style_generation() const { return m_animation_style_generation; }
    u64 animation_subtree_style_generation() const { return m_animation_subtree_style_generation; }
    [[nodiscard]] bool has_style(Optional<CSS::PseudoElement> pseudo_element = {}) const { return !!style_record_identity(pseudo_element); }
    [[nodiscard]] void const* style_record_payloads(Optional<CSS::PseudoElement> = {}) const;
    template<typename StyleGroup>
    StyleGroup const* style_group(Optional<CSS::PseudoElement> pseudo_element = {}) const
    {
        auto const* payloads = static_cast<void const* const*>(style_record_payloads(pseudo_element));
        if (!payloads)
            return nullptr;
        auto const* payload = payloads[StyleGroup::style_group_index];
        VERIFY(payload);
        return static_cast<StyleGroup const*>(payload);
    }
    void set_computed_style(Optional<CSS::PseudoElement>, CSS::StyleRecordID);
    void refresh_computed_style(Optional<CSS::PseudoElement>, CSS::StyleRecordID);
    void update_animated_properties(Badge<Web::Animations::KeyframeEffect> const&, Optional<CSS::PseudoElement>, Web::Animations::KeyframeEffect&, Web::Animations::AnimationUpdateContext&);
    void update_animated_properties_for_abstract_element(Badge<Web::Animations::KeyframeEffect> const&, DOM::AbstractElement, Web::Animations::KeyframeEffect&, Web::Animations::AnimationUpdateContext&);

    Optional<SyntheticPseudoElement&> get_synthetic_pseudo_element(CSS::PseudoElement) const;
    Optional<PseudoElement&> get_pseudo_element(CSS::PseudoElement) const;

    template<typename Callback>
    void for_each_synthetic_pseudo_element(Callback const& callback)
    {
        auto* pseudo_element_data = this->pseudo_element_data();
        if (!pseudo_element_data)
            return;

        for (auto i = to_underlying(CSS::first_synthetic_pseudo_element); i <= to_underlying(CSS::last_synthetic_pseudo_element); ++i) {
            auto type = static_cast<CSS::PseudoElement>(i);
            auto pseudo_element = pseudo_element_data->get(type);
            if (!pseudo_element.has_value())
                continue;

            using ReturnType = InvokeResult<Callback, CSS::PseudoElement, SyntheticPseudoElement&>;
            if constexpr (IsSame<ReturnType, IterationDecision>) {
                if (callback(type, as<SyntheticPseudoElement>(*pseudo_element.release_value())) == IterationDecision::Break)
                    return;
            } else {
                static_assert(IsSame<ReturnType, void>);
                callback(type, as<SyntheticPseudoElement>(*pseudo_element.release_value()));
            }
        }
    }

    template<typename Callback>
    void for_each_synthetic_pseudo_element(Callback const& callback) const
    {
        auto const* pseudo_element_data = this->pseudo_element_data();
        if (!pseudo_element_data)
            return;

        for (auto i = to_underlying(CSS::first_synthetic_pseudo_element); i <= to_underlying(CSS::last_synthetic_pseudo_element); ++i) {
            auto type = static_cast<CSS::PseudoElement>(i);
            auto pseudo_element = pseudo_element_data->get(type);
            if (!pseudo_element.has_value())
                continue;

            using ReturnType = InvokeResult<Callback, CSS::PseudoElement, SyntheticPseudoElement&>;
            if constexpr (IsSame<ReturnType, IterationDecision>) {
                if (callback(type, as<SyntheticPseudoElement>(*pseudo_element.release_value())) == IterationDecision::Break)
                    return;
            } else {
                static_assert(IsSame<ReturnType, void>);
                callback(type, as<SyntheticPseudoElement>(*pseudo_element.release_value()));
            }
        }
    }

    GC::Ptr<CSS::CSSStyleProperties> inline_style() { return m_inline_style; }
    GC::Ptr<CSS::CSSStyleProperties const> inline_style() const { return m_inline_style; }
    void set_inline_style(GC::Ptr<CSS::CSSStyleProperties>);
    void prepare_for_inline_style_change();
    bool can_defer_inline_style_attribute_update() const;
    void did_update_inline_style();

    GC::Ref<CSS::CSSStyleProperties> style();
    GC::Ref<CSS::StylePropertyMap> attribute_style_map();

    CSS::StyleSheetList& document_or_shadow_root_style_sheets();
    ElementByIdMap& document_or_shadow_root_element_by_id_map();

    static WebIDL::ExceptionOr<GC::Ref<DOM::DocumentFragment>> parse_fragment(Variant<GC::Ref<Element>, GC::Ref<DocumentFragment>> target, Utf16View markup, HTML::ParserScriptingMode = HTML::ParserScriptingMode::Inert);

    [[nodiscard]] GC::Ptr<Element const> element_to_inherit_style_from(Optional<CSS::PseudoElement>) const;

    WebIDL::ExceptionOr<Utf16String> inner_html() const;
    WebIDL::ExceptionOr<void> set_inner_html(Utf16View html);

    WebIDL::ExceptionOr<void> set_html_unsafe(StringView html);

    WebIDL::ExceptionOr<Utf16String> get_html(HTMLSerializationOptions const&) const;

    WebIDL::ExceptionOr<void> insert_adjacent_html(String const& position, StringView html);

    enum class FullscreenRequester {
        Bindings,
        WebDriver,
    };
    void request_fullscreen(GC::Ptr<WebIDL::Promise>, FullscreenRequester = FullscreenRequester::Bindings, Fullscreen::RequestType = Fullscreen::RequestType::Standard);
    void webkit_request_fullscreen();

    RequestFullscreenError is_element_allowed_to_enter_fullscreen(FullscreenRequester) const;
    bool is_element_ready_for_fullscreen() const;

    void set_fullscreen_flag(bool is_fullscreen);
    bool is_fullscreen_element() const { return m_fullscreen_flag; }
    void set_fullscreen_request_type(Fullscreen::RequestType);
    Fullscreen::RequestType fullscreen_request_type() const;

    GC::Ptr<WebIDL::CallbackType> onfullscreenchange();
    void set_onfullscreenchange(GC::Ptr<WebIDL::CallbackType>);

    GC::Ptr<WebIDL::CallbackType> onfullscreenerror();
    void set_onfullscreenerror(GC::Ptr<WebIDL::CallbackType>);

    GC::Ptr<WebIDL::CallbackType> onwebkitfullscreenchange();
    void set_onwebkitfullscreenchange(GC::Ptr<WebIDL::CallbackType>);

    GC::Ptr<WebIDL::CallbackType> onwebkitfullscreenerror();
    void set_onwebkitfullscreenerror(GC::Ptr<WebIDL::CallbackType>);

    WebIDL::ExceptionOr<Utf16String> outer_html() const;
    WebIDL::ExceptionOr<void> set_outer_html(StringView html);

    bool is_focused() const;
    bool is_the_active_element() const;

    bool is_being_activated() const;
    virtual void set_being_activated(bool);

    bool is_target() const;
    bool is_document_element() const;

    bool is_shadow_host() const;
    GC::Ptr<ShadowRoot> shadow_root() { return m_shadow_root; }
    GC::Ptr<ShadowRoot const> shadow_root() const { return m_shadow_root; }
    void set_shadow_root(GC::Ptr<ShadowRoot>);

    void set_custom_property_data(Optional<CSS::PseudoElement>, RefPtr<CSS::CustomPropertyData const>);
    void replace_custom_property_data(Optional<CSS::PseudoElement>, RefPtr<CSS::CustomPropertyData const>);
    [[nodiscard]] RefPtr<CSS::CustomPropertyData const> custom_property_data(Optional<CSS::PseudoElement>) const;

    [[nodiscard]] bool refresh_inherited_custom_property_data();

    // What the element's last computation was allowed to read, so a later one can ask whether any of
    // it moved before deriving a style that would be identical. Retired by anything that moves what
    // a word of it names without moving the name, which is a write to a declaration the element
    // itself sources.
    // What the custom-property name index was last told about this element. Publishing it again
    // costs one atom per name and a crossing into the engine, and a recomputation that resolved the
    // same environment has the same names to publish.
    struct PublishedCustomPropertyNames {
        RefPtr<CSS::CustomPropertyData const> data;
        bool uses_var_css_function { false };
        bool uses_custom_function { false };
        bool operator==(PublishedCustomPropertyNames const&) const = default;
    };
    struct PseudoElementStyleQueryCustomPropertyReferences {
        CSS::PseudoElement pseudo_element;
        Vector<Utf16FlyString> references;
    };
    [[nodiscard]] CSS::StyleInputRecord const* style_input_record() const { return m_style_input_record.ptr(); }
    [[nodiscard]] CSS::StyleInputRecord* style_input_record() { return m_style_input_record.ptr(); }
    void set_style_input_record(OwnPtr<CSS::StyleInputRecord>);
    void retire_style_input_record();
    [[nodiscard]] OwnPtr<CSS::StyleInputRecord> take_style_input_record();
    void record_style_custom_property_reference(Utf16FlyString const&);
    void record_style_query_custom_property_reference(Optional<CSS::PseudoElement>, Utf16FlyString const&);
    void finish_recording_style_custom_property_references();

    bool style_uses_attr_css_function() const { return m_style_uses_attr_css_function; }
    void set_style_uses_attr_css_function() { m_style_uses_attr_css_function = true; }
    bool style_uses_var_css_function() const { return m_style_uses_var_css_function; }
    void set_style_uses_var_css_function() { m_style_uses_var_css_function = true; }
    // A tree-counting function is answered from the element's position among its siblings, so what
    // has to be remembered is on the parent: a child list mutation there moves the answer.
    void set_style_uses_tree_counting_function()
    {
        m_style_uses_tree_counting_function = true;
        if (auto parent = parent_element())
            parent->set_child_style_uses_tree_counting_function();
    }
    // Whether this element's own style read its place among its siblings, which is what makes its
    // computed values its own rather than a function of what decides for it.
    bool style_uses_tree_counting_function() const { return m_style_uses_tree_counting_function; }
    // Whether this element's style resolution called a custom function. Which one is not reported by
    // the substitution machinery, so an `@function` change reaches the elements that called any.
    void set_style_uses_custom_function() { m_style_uses_custom_function = true; }
    bool style_uses_custom_function() const { return m_style_uses_custom_function; }

    bool style_uses_if_css_function() const { return m_style_uses_if_css_function; }
    void set_style_uses_if_css_function() { m_style_uses_if_css_function = true; }
    bool style_depends_on_viewport_metrics() const { return m_style_depends_on_viewport_metrics; }
    void set_style_depends_on_viewport_metrics() { m_style_depends_on_viewport_metrics = true; }
    bool style_uses_inherit_css_function() const { return m_style_uses_inherit_css_function; }
    void set_style_uses_inherit_css_function() { m_style_uses_inherit_css_function = true; }
    bool style_depends_on_size_container_query() const { return m_style_depends_on_size_container_query; }
    void set_style_depends_on_size_container_query() { m_style_depends_on_size_container_query = true; }
    bool style_depends_on_style_container_query() const { return m_style_depends_on_style_container_query; }
    void set_style_depends_on_style_container_query() { m_style_depends_on_style_container_query = true; }
    // Set on the element a container query selected as its query container, so a change on it knows
    // whether anything under it was ever asking. Neither is ever cleared: a dependent that stops
    // asking republishes nothing, and answering "maybe" costs the scan the element used to pay
    // unconditionally.
    void set_is_style_query_container() { m_is_style_query_container = true; }
    bool is_style_query_container() const { return m_is_style_query_container; }
    bool is_size_query_container() const { return m_is_size_query_container; }
    void set_is_size_query_container() { m_is_size_query_container = true; }
    void invalidate_descendant_styles_depending_on_style_container_query();

    bool child_style_uses_tree_counting_function() const { return m_child_style_uses_tree_counting_function; }
    void set_child_style_uses_tree_counting_function() { m_child_style_uses_tree_counting_function = true; }

    // NOTE: The function is wrapped in a GC::HeapFunction immediately.
    HTML::TaskID queue_an_element_task(HTML::Task::Source, Function<void()>);

    bool is_void_element() const;
    bool serializes_as_void() const;

    [[nodiscard]] CSSPixelRect get_bounding_client_rect() const;

    [[nodiscard]] Vector<CSSPixelRect> get_client_rects() const;

    [[nodiscard]] CSSPixelRect bounding_client_rect_assuming_layout_clean() const;
    [[nodiscard]] CSSPixelRect bounding_client_rect_assuming_layout_clean(Painting::AccumulatedVisualContextTree const&) const;

    virtual Layout::Node* create_layout_node(CSS::LayoutStyle);

    virtual void did_receive_focus() { }
    virtual void did_lose_focus() { }
    bool should_indicate_focus() const;
    virtual bool is_focusable() const override;

    static Layout::NodeWithStyle* create_layout_node_for_display_type(DOM::Document&, CSS::Display const&, CSS::LayoutStyle, Element*);

    void set_synthetic_pseudo_element_node(Badge<Layout::LayoutTreeBuilderAccess>, CSS::PseudoElement, Layout::NodeWithStyle*);

    Layout::NodeWithStyle* pseudo_element_layout_node(CSS::PseudoElement) const;
    Layout::NodeWithStyle* pseudo_element_unsafe_layout_node(CSS::PseudoElement) const;

    bool has_synthetic_pseudo_elements() const;
    void clear_synthetic_pseudo_element_layout_nodes(Badge<Layout::LayoutTreeBuilderAccess, Node>) { clear_synthetic_pseudo_element_layout_nodes(); }

    void serialize_children_as_json(JsonObjectSerializer<Utf16StringBuilder>&) const;

    i32 tab_index() const;
    void set_tab_index(i32 tab_index);

    enum class TreatOverflowClipOnBodyParentAsOverflowHidden {
        No,
        Yes,
    };
    bool is_potentially_scrollable(TreatOverflowClipOnBodyParentAsOverflowHidden) const;
    bool is_scroll_container() const;

    double scroll_top() const;
    double scroll_left() const;
    void set_scroll_top(double y);
    void set_scroll_left(double x);

    int scroll_width();
    int scroll_height();

    bool is_actually_disabled() const;

    WebIDL::ExceptionOr<GC::Ptr<Element>> insert_adjacent_element(Utf16View where, GC::Ref<Element> element);
    WebIDL::ExceptionOr<void> insert_adjacent_text(Utf16View where, Utf16View data);

    using ScrollBehavior = Bindings::ScrollBehavior;
    using ScrollLogicalPosition = Bindings::ScrollLogicalPosition;
    using ScrollIntoViewContainer = Bindings::ScrollIntoViewContainer;
    using ScrollIntoViewOptions = Bindings::ScrollIntoViewOptions;

    // https://w3c.github.io/csswg-drafts/cssom-view-1/#dom-element-scrollintoview
    void scroll_into_view(Variant<bool, ScrollIntoViewOptions> const&, GC::Ptr<WebIDL::Promise>);
    void scroll_into_view(ScrollIntoViewOptions const&, GC::Ptr<WebIDL::Promise>);

    // https://www.w3.org/TR/wai-aria-1.2/#ARIAMixin
#define __ENUMERATE_ARIA_ATTRIBUTE(name, attribute)      \
    virtual Optional<Utf16String> name() const override; \
    virtual void set_##name(Optional<Utf16String> const& value) override;
    ENUMERATE_ARIA_ATTRIBUTES
#undef __ENUMERATE_ARIA_ATTRIBUTE

    virtual bool exclude_from_accessibility_tree() const override;

    virtual bool include_in_accessibility_tree() const override;

    virtual Element& to_element() override { return *this; }
    virtual Element const& to_element() const override { return *this; }

    bool is_hidden() const;
    bool has_hidden_ancestor() const;

    bool is_referenced() const;
    bool has_referenced_and_hidden_ancestor() const;

    void enqueue_a_custom_element_upgrade_reaction(HTML::CustomElementDefinition& custom_element_definition);
    void enqueue_a_custom_element_callback_reaction(Utf16FlyString const& callback_name);
    void enqueue_an_adopted_callback_reaction(Document& old_document, Document& new_document);
    void enqueue_an_attribute_changed_callback_reaction(Utf16FlyString const& attribute_name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& new_value, Optional<Utf16FlyString> const& namespace_uri);
    void enqueue_a_form_associated_callback_reaction(GC::Ptr<HTML::HTMLFormElement> form);
    void enqueue_a_form_disabled_callback_reaction(bool is_disabled);
    void enqueue_a_custom_element_callback_reaction(Utf16FlyString const& callback_name, CustomElementCallbackReactionArguments arguments);

    using CustomElementReactionQueue = Vector<Variant<CustomElementUpgradeReaction, CustomElementCallbackReaction, CustomElementConnectedMoveCallbackReaction>>;
    CustomElementReactionQueue* custom_element_reaction_queue();
    CustomElementReactionQueue const* custom_element_reaction_queue() const;
    CustomElementReactionQueue& ensure_custom_element_reaction_queue();

    GC::Ptr<HTML::CustomStateSet const> custom_state_set() const;
    HTML::CustomStateSet& ensure_custom_state_set();

    bool can_upgrade_custom_element() const { return m_custom_element_state == CustomElementState::Undefined || m_custom_element_state == CustomElementState::Uncustomized; }
    void try_to_upgrade();

    bool is_defined() const;
    bool is_custom() const;

    Optional<Utf16FlyString> const& is_value() const;
    void set_is_value(Optional<Utf16FlyString> const& is);

    void set_custom_element_state(CustomElementState);
    void set_custom_element_definition(GC::Ptr<HTML::CustomElementDefinition>);
    void clear_custom_element_reaction_queue();
    void setup_custom_element_from_constructor(HTML::CustomElementDefinition& custom_element_definition, Optional<Utf16FlyString> const& is_value);

    using ScrollToOptions = Bindings::ScrollToOptions;

    void scroll(ScrollToOptions, GC::Ptr<WebIDL::Promise>, Optional<CSSPixelPoint> relative_displacement = {});
    void scroll(double x, double y, GC::Ptr<WebIDL::Promise>);
    void scroll_by(ScrollToOptions, GC::Ptr<WebIDL::Promise>);
    void scroll_by(double x, double y, GC::Ptr<WebIDL::Promise>);

    using CheckVisibilityOptions = Bindings::CheckVisibilityOptions;

    bool check_visibility(CheckVisibilityOptions const&);

    void register_intersection_observer(Badge<IntersectionObserver::IntersectionObserver>, GC::Ref<IntersectionObserver::IntersectionObserver>);
    void unregister_intersection_observer(Badge<IntersectionObserver::IntersectionObserver>, GC::Ref<IntersectionObserver::IntersectionObserver>);

    CSSPixelPoint scroll_offset(Optional<CSS::PseudoElement> type) const;
    void set_scroll_offset(Optional<CSS::PseudoElement> type, CSSPixelPoint offset);

    enum class TranslationMode {
        TranslateEnabled,
        NoTranslate
    };
    TranslationMode translation_mode() const;

    enum class Dir {
        Ltr,
        Rtl,
        Auto,
    };
    Optional<Dir> dir() const;
    bool has_auto_directionality() const;

    enum class Directionality {
        Ltr,
        Rtl,
    };
    Directionality directionality() const;
    bool is_auto_directionality_form_associated_element() const;

    Optional<Utf16FlyString> const& id() const { return m_id; }
    Optional<Utf16FlyString> name() const;

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

    bool has_non_empty_counters_set() const;
    Optional<CSS::CountersSet const&> counters_set() const;
    CSS::CountersSet& ensure_counters_set();
    void set_counters_set(OwnPtr<CSS::CountersSet>&&);

    ProximityToTheViewport proximity_to_the_viewport() const;
    void determine_proximity_to_the_viewport();
    bool is_relevant_to_the_user();

    // https://drafts.csswg.org/css-contain-2/#skips-its-contents
    bool skips_its_contents();

    bool matches_enabled_pseudo_class() const;
    bool matches_disabled_pseudo_class() const;
    bool matches_checked_pseudo_class() const;
    bool matches_unchecked_pseudo_class() const;
    bool matches_placeholder_shown_pseudo_class() const;
    bool matches_link_pseudo_class() const;
    bool matches_visited_pseudo_class() const;
    bool matches_local_link_pseudo_class() const;
    bool matches_focus_within_pseudo_class() const;

    void schedule_list_item_renumber_for_list_owner();
    bool list_item_renumber_affects_rendered_content() const;
    bool after_pseudo_element_style_depends_on_list_item_counter() const;

    bool captured_in_a_view_transition() const;
    void set_captured_in_a_view_transition(bool);

    // https://drafts.csswg.org/css-images-4/#element-not-rendered
    bool not_rendered() const;

    bool meets_focusable_area_rendering_requirements() const;

    // https://drafts.csswg.org/css-view-transitions-1/#document-scoped-view-transition-name
    Optional<Utf16FlyString> document_scoped_view_transition_name();

    // https://drafts.csswg.org/css-view-transitions-1/#capture-the-image
    Optional<Gfx::DecodedImageFrame> capture_the_image();

    void set_pointer_capture(WebIDL::Long pointer_id);
    void release_pointer_capture(WebIDL::Long pointer_id);
    bool has_pointer_capture(WebIDL::Long pointer_id);

    virtual bool contributes_a_script_blocking_style_sheet() const { return false; }

    void set_had_duplicate_attribute_during_tokenization(Badge<HTML::HTMLParser>);
    bool had_duplicate_attribute_during_tokenization() const;

    GC::Ref<CSS::StylePropertyMapReadOnly> computed_style_map();

    // https://html.spec.whatwg.org/multipage/dom.html#block-rendering
    void block_rendering();
    // https://html.spec.whatwg.org/multipage/dom.html#unblock-rendering
    void unblock_rendering();
    // https://html.spec.whatwg.org/multipage/urls-and-fetching.html#potentially-render-blocking
    bool is_potentially_render_blocking();
    // https://html.spec.whatwg.org/multipage/urls-and-fetching.html#implicitly-potentially-render-blocking
    virtual bool is_implicitly_potentially_render_blocking() const { return false; }

    double ensure_css_random_base_value(CSS::RandomCachingKey const&);

    struct PointerLockOptions {
        bool unadjusted_movement { false };
    };

    WebIDL::ExceptionOr<void> request_pointer_lock(PointerLockOptions const&);

    GC::Ptr<HTML::CustomElementRegistry> custom_element_registry() const;
    void set_custom_element_registry(GC::Ptr<HTML::CustomElementRegistry>);

    virtual void initialize_element() { }

    void prepare_for_style_computation(Badge<CSS::StyleComputer>) { prepare_for_style_computation(); }

protected:
    Element(Document&, DOM::QualifiedName);

    virtual void inserted() override;
    virtual void removed_from(IsSubtreeRoot, Node* old_ancestor, Node& old_root) override;
    virtual void moved_from(IsSubtreeRoot, GC::Ptr<Node> old_ancestor) override;

    virtual void children_changed(ChildrenChangedMetadata const&) override;
    virtual i32 default_tab_index_value() const;

    // https://dom.spec.whatwg.org/#concept-element-attributes-change-ext
    MUST_UPCALL virtual void attribute_changed(Utf16FlyString const& local_name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_);

    virtual void computed_properties_changed() { }
    virtual void prepare_for_style_computation() { }

    virtual void visit_edges(Cell::Visitor&) override;
    virtual size_t external_memory_size() const override;

    virtual bool id_reference_exists(Utf16View) const override;

    CustomElementState custom_element_state() const { return m_custom_element_state; }
    GC::Ptr<HTML::CustomElementDefinition> custom_element_definition() const;

    friend void Bindings::set_prototype_from_custom_element_definition_if_needed(Element&, Bindings::PlatformObject&);
    template<typename>
    friend class HTML::HTMLOrSVGOrMathMLElement;

    void play_or_cancel_animations_after_display_property_change();
    void clear_element_reference_pseudo_elements();

    struct RareData;

private:
    struct Attribute {
        QualifiedName name;
        Utf16String value;
    };
    using AttributeList = Vector<Attribute, 1>;

    AttributeList& ensure_attribute_list();

    void install_custom_property_data(Optional<CSS::PseudoElement>, RefPtr<CSS::CustomPropertyData const>);
    void synchronize_attribute(Utf16FlyString const& qualified_name) const;
    void synchronize_attribute_ns(Optional<Utf16FlyString> const&, Utf16FlyString const& local_name) const;
    void synchronize_style_attribute() const;
    Optional<size_t> find_attribute_index(Utf16FlyString const& qualified_name) const;
    Optional<size_t> find_attribute_index_ns(Optional<Utf16FlyString> const&, Utf16FlyString const& local_name) const;
    void change_attribute_value(GC::Ref<Attr>, Utf16String value);
    void handle_attribute_changes(QualifiedName, Optional<Utf16String> old_value, Optional<Utf16String> new_value);
    void remove_attribute_at(size_t index);

    using PreservedPseudoElementStyles = Array<CSS::StyleRecordID, to_underlying(CSS::PseudoElement::KnownPseudoElementCount)>;
    using PseudoElementData = HashMap<CSS::PseudoElement, GC::Ref<PseudoElement>>;

    virtual OwnPtr<Node::RareData> create_rare_data() const override;
    virtual SlottableMixin::RareData* slottable_rare_data() override;
    virtual SlottableMixin::RareData const* slottable_rare_data() const override;
    virtual SlottableMixin::RareData& ensure_slottable_rare_data() override;
    virtual ARIA::ARIAMixin::RareData* aria_rare_data() override;
    virtual ARIA::ARIAMixin::RareData const* aria_rare_data() const override;
    virtual ARIA::ARIAMixin::RareData& ensure_aria_rare_data() override;
    RareData& ensure_element_rare_data() const;
    RareData* element_rare_data();
    RareData const* element_rare_data() const;
    PseudoElementData* pseudo_element_data();
    PseudoElementData const* pseudo_element_data() const;

    Utf16FlyString make_html_uppercased_qualified_name() const;

    void exit_fullscreen_on_element_removal();
    CSS::RequiredInvalidationAfterStyleChange recompute_pseudo_element_styles(bool& did_change_custom_properties, bool had_list_marker, CSS::ComputedValues const* old_originating_style, CSS::StyleEngineMatchResult* = nullptr, PreservedPseudoElementStyles* = nullptr, EnginePseudoElementRecords const* = nullptr);
    void apply_computed_style_to_layout_node_if_needed(CSS::RequiredInvalidationAfterStyleChange const&);
    void apply_computed_pseudo_element_styles_to_layout_nodes_if_needed(CSS::RequiredInvalidationAfterStyleChange const&);
    void publish_custom_property_names();
    void replace_style_record(CSS::StyleRecordID);
    void clear_computed_styles_from_display_none_descendants();

    WebIDL::ExceptionOr<GC::Ptr<Node>> insert_adjacent(Utf16View where, GC::Ref<Node> node);

    void enqueue_an_element_on_the_appropriate_element_queue();

    Optional<Directionality> auto_directionality() const;
    Optional<Directionality> contained_text_auto_directionality(bool can_exclude_root) const;
    Directionality parent_directionality() const;

    QualifiedName m_qualified_name;

    OwnPtr<AttributeList> m_attributes;
    GC::Ptr<CSS::CSSStyleProperties> m_inline_style;
    GC::Ptr<ShadowRoot> m_shadow_root;

    // A consumer handle mirroring StyleEngine's authoritative style-record column. C++ consumers
    // borrow the record-owned computed-values view rather than retaining one complete style per
    // element.
    CSS::StyleRecordID m_style_record_identity;
    u64 m_animation_style_generation { 0 };
    u64 m_animation_subtree_style_generation { 0 };
    RefPtr<CSS::CustomPropertyData const> m_custom_property_data;
    OwnPtr<CSS::StyleInputRecord> m_style_input_record;
    PublishedCustomPropertyNames m_published_custom_property_names;
    Vector<CSS::StyleProperty> m_published_presentational_hint_properties;

    void register_element_reference_pseudo_element(CSS::PseudoElement type, GC::Ref<Element> element);
    SyntheticPseudoElement& ensure_synthetic_pseudo_element(CSS::PseudoElement) const;
    void clear_synthetic_pseudo_element_layout_nodes();

    Vector<Utf16FlyString> m_classes;
    CSS::StyleNodeID m_style_node_id;

    Optional<Utf16FlyString> m_id;

    friend class Attr;
    friend class NamedNodeMap;

    bool m_is_being_activated : 1 { false };
    bool m_in_top_layer : 1 { false };
    bool m_rendered_in_top_layer : 1 { false };
    // Authoritative dependency marks left by this element's latest style computation.
    bool m_style_uses_attr_css_function : 1 { false };
    bool m_style_uses_var_css_function : 1 { false };
    bool m_style_uses_if_css_function : 1 { false };
    bool m_style_depends_on_viewport_metrics : 1 { false };
    bool m_style_uses_custom_function : 1 { false };
    bool m_style_uses_inherit_css_function : 1 { false };
    bool m_style_depends_on_size_container_query : 1 { false };
    bool m_style_depends_on_style_container_query : 1 { false };
    bool m_is_style_query_container : 1 { false };
    bool m_is_size_query_container : 1 { false };
    bool m_child_style_uses_tree_counting_function : 1 { false };
    bool m_style_uses_tree_counting_function : 1 { false };
    bool m_fullscreen_flag : 1 { false };
    bool m_uses_document_global_custom_element_registry : 1 { false };
    bool m_has_name : 1 { false };
    mutable bool m_style_attribute_is_dirty : 1 { false };

    mutable Optional<Utf16String> m_lang_value;

    // https://dom.spec.whatwg.org/#concept-element-custom-element-state
    CustomElementState m_custom_element_state { CustomElementState::Undefined };
};

template<>
inline bool Node::fast_is<Element>() const { return is_element(); }

inline GC::Ptr<Element> Node::parent_element()
{
    return as_if<Element>(this->parent());
}

inline GC::Ptr<Element const> Node::parent_element() const
{
    return as_if<Element>(this->parent());
}

inline bool Element::has_class(Utf16View class_name, CaseSensitivity case_sensitivity) const
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

inline bool Element::has_class(Utf16FlyString const& class_name, CaseSensitivity case_sensitivity) const
{
    if (case_sensitivity == CaseSensitivity::CaseSensitive) {
        // NB: Comparing two interned strings is a pointer comparison.
        return any_of(m_classes, [&](auto& it) {
            return it == class_name;
        });
    }
    return any_of(m_classes, [&](auto& it) {
        return it.equals_ignoring_ascii_case(class_name);
    });
}

bool is_valid_namespace_prefix(Utf16View);
bool is_valid_attribute_local_name(Utf16View);
bool is_valid_element_local_name(Utf16View const&);

enum class ValidationContext {
    Attribute,
    Element,
};

enum class ValidateAndExtractError : u8 {
    InvalidNamespacePrefix,
    InvalidAttributeLocalName,
    InvalidElementLocalName,
    PrefixWithNullNamespace,
    XMLPrefixWithNonXMLNamespace,
    XMLNSPrefixWithNonXMLNSNamespace,
    XMLNSNamespaceWithoutXMLNSPrefix,
};

ErrorOr<QualifiedName, ValidateAndExtractError> validate_and_extract(Optional<FlyString> namespace_, FlyString const& qualified_name, ValidationContext context);
GC::Ref<WebIDL::DOMException> validate_and_extract_error_to_dom_exception(ValidateAndExtractError);

}
