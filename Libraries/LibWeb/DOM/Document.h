/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2023-2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <AK/WeakPtr.h>
#include <LibCore/Forward.h>
#include <LibCore/SharedVersion.h>
#include <LibJS/Forward.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>
#include <LibUnicode/Forward.h>
#include <LibWeb/CSS/CustomPropertyRegistration.h>
#include <LibWeb/CSS/EnvironmentVariable.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/DOM/ParentNode.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/ViewportClient.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/CrossOrigin/OpenerPolicy.h>
#include <LibWeb/HTML/DocumentReadyState.h>
#include <LibWeb/HTML/Focus.h>
#include <LibWeb/HTML/NavigationType.h>
#include <LibWeb/HTML/PaintConfig.h>
#include <LibWeb/HTML/SandboxingFlagSet.h>
#include <LibWeb/HTML/VisibilityState.h>
#include <LibWeb/InvalidateDisplayList.h>
#include <LibWeb/ResizeObserver/ResizeObserver.h>
#include <LibWeb/TrustedTypes/InjectionSink.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::DOM {

enum class QuirksMode {
    No,
    Limited,
    Yes
};

#define ENUMERATE_INVALIDATE_LAYOUT_TREE_REASONS(X)       \
    X(DocumentAddAnElementToTheTopLayer)                  \
    X(DocumentRequestAnElementToBeRemovedFromTheTopLayer) \
    X(DocumentImmediatelyRemoveElementFromTheTopLayer)    \
    X(DocumentPendingTopLayerRemovalsProcessed)           \
    X(ShadowRootSetInnerHTML)

enum class InvalidateLayoutTreeReason {
#define ENUMERATE_INVALIDATE_LAYOUT_TREE_REASON(e) e,
    ENUMERATE_INVALIDATE_LAYOUT_TREE_REASONS(ENUMERATE_INVALIDATE_LAYOUT_TREE_REASON)
#undef ENUMERATE_INVALIDATE_LAYOUT_TREE_REASON
};

[[nodiscard]] StringView to_string(InvalidateLayoutTreeReason);

#define ENUMERATE_UPDATE_LAYOUT_REASONS(X) \
    X(AutoScrollSelection)                 \
    X(ChildDocumentStyleUpdate)            \
    X(Debugging)                           \
    X(DocumentElementFromPoint)            \
    X(DocumentElementsFromPoint)           \
    X(DocumentFindMatchingText)            \
    X(DocumentSetDesignMode)               \
    X(DumpDisplayList)                     \
    X(ElementCheckVisibility)              \
    X(ElementClientHeight)                 \
    X(ElementClientWidth)                  \
    X(ElementGetClientRects)               \
    X(ElementIsPotentiallyScrollable)      \
    X(ElementScroll)                       \
    X(ElementScrollHeight)                 \
    X(ElementScrollIntoView)               \
    X(ElementScrollLeft)                   \
    X(ElementScrollTop)                    \
    X(ElementScrollWidth)                  \
    X(ElementSetScrollLeft)                \
    X(ElementSetScrollTop)                 \
    X(EventHandlerHandleDoubleClick)       \
    X(EventHandlerHandleDragAndDrop)       \
    X(EventHandlerHandleMouseDown)         \
    X(EventHandlerHandleMouseMove)         \
    X(EventHandlerHandleMouseUp)           \
    X(EventHandlerHandleMouseWheel)        \
    X(EventHandlerHandleTripleClick)       \
    X(HTMLElementGetTheTextSteps)          \
    X(HTMLElementOffsetHeight)             \
    X(HTMLElementOffsetLeft)               \
    X(HTMLElementOffsetParent)             \
    X(HTMLElementOffsetTop)                \
    X(HTMLElementOffsetWidth)              \
    X(HTMLElementScrollParent)             \
    X(HTMLEventLoopRenderingUpdate)        \
    X(HTMLImageElementHeight)              \
    X(HTMLImageElementWidth)               \
    X(HTMLImageElementX)                   \
    X(HTMLImageElementY)                   \
    X(HTMLInputElementHeight)              \
    X(HTMLInputElementWidth)               \
    X(InspectDOMTree)                      \
    X(InternalsHitTest)                    \
    X(MediaQueryListMatches)               \
    X(NavigableSelectedText)               \
    X(NavigableViewportScroll)             \
    X(NodeNameOrDescription)               \
    X(RangeGetClientRects)                 \
    X(ResolvedCSSStyleDeclarationProperty) \
    X(SVGDecodedImageDataRender)           \
    X(ScrollCursorIntoView)                \
    X(ProcessScreenshot)                   \
    X(SVGGraphicsElementGetBBox)           \
    X(SourceSetNormalizeSourceDensities)   \
    X(ViewTransitionCapture)               \
    X(WindowScroll)

enum class UpdateLayoutReason {
#define ENUMERATE_UPDATE_LAYOUT_REASON(e) e,
    ENUMERATE_UPDATE_LAYOUT_REASONS(ENUMERATE_UPDATE_LAYOUT_REASON)
#undef ENUMERATE_UPDATE_LAYOUT_REASON
};

[[nodiscard]] StringView to_string(UpdateLayoutReason);

// https://html.spec.whatwg.org/multipage/dom.html#document-load-timing-info
struct DocumentLoadTimingInfo {
    // https://html.spec.whatwg.org/multipage/dom.html#navigation-start-time
    double navigation_start_time { 0 };
    // https://html.spec.whatwg.org/multipage/dom.html#dom-interactive-time
    HighResolutionTime::DOMHighResTimeStamp dom_interactive_time { 0 };
    // https://html.spec.whatwg.org/multipage/dom.html#dom-content-loaded-event-start-time
    HighResolutionTime::DOMHighResTimeStamp dom_content_loaded_event_start_time { 0 };
    // https://html.spec.whatwg.org/multipage/dom.html#dom-content-loaded-event-end-time
    HighResolutionTime::DOMHighResTimeStamp dom_content_loaded_event_end_time { 0 };
    // https://html.spec.whatwg.org/multipage/dom.html#dom-complete-time
    HighResolutionTime::DOMHighResTimeStamp dom_complete_time { 0 };
    // https://html.spec.whatwg.org/multipage/dom.html#load-event-start-time
    HighResolutionTime::DOMHighResTimeStamp load_event_start_time { 0 };
    // https://html.spec.whatwg.org/multipage/dom.html#load-event-end-time
    HighResolutionTime::DOMHighResTimeStamp load_event_end_time { 0 };
};

// https://html.spec.whatwg.org/multipage/dom.html#document-unload-timing-info
struct DocumentUnloadTimingInfo {
    // https://html.spec.whatwg.org/multipage/dom.html#unload-event-start-time
    double unload_event_start_time { 0 };
    // https://html.spec.whatwg.org/multipage/dom.html#unload-event-end-time
    double unload_event_end_time { 0 };
};

struct ElementCreationOptions {
    Optional<String> is;
};

enum class PolicyControlledFeature : u8 {
    Autoplay,
    EncryptedMedia,
    FocusWithoutUserActivation,
    Fullscreen,
    Gamepad,
    WindowManagement,
};

struct PendingFullscreenEvent {
    enum class Type {
        Change,
        Error,
    } type;
    GC::Ref<Element> element;
};

class WEB_API Document
    : public ParentNode
    , public HTML::GlobalEventHandlers {
    WEB_PLATFORM_OBJECT(Document, ParentNode);
    GC_DECLARE_ALLOCATOR(Document);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    enum class Type {
        XML,
        HTML
    };

    enum class TemporaryDocumentForFragmentParsing {
        No,
        Yes,
    };

    static WebIDL::ExceptionOr<GC::Ref<Document>> create_and_initialize(Type, String content_type, HTML::NavigationParams const&);

    [[nodiscard]] static GC::Ref<Document> create(JS::Realm&, URL::URL const& url = URL::about_blank());
    [[nodiscard]] static GC::Ref<Document> create_for_fragment_parsing(JS::Realm&);
    static GC::Ref<Document> construct_impl(JS::Realm&);
    virtual ~Document() override;

    // AD-HOC: This number increments whenever a node is added or removed from the document, or an element attribute changes.
    //         It can be used as a crude invalidation mechanism for caches that depend on the DOM structure.
    u64 dom_tree_version() const { return m_dom_tree_version; }
    void bump_dom_tree_version() { ++m_dom_tree_version; }

    // AD-HOC: This number increments whenever CharacterData is modified in the document. It is used together with
    //         dom_tree_version() to understand whether either the DOM tree structure or contents were changed.
    u64 character_data_version() const { return m_character_data_version; }
    void bump_character_data_version() { ++m_character_data_version; }

    WebIDL::ExceptionOr<void> populate_with_html_head_and_body();

    GC::Ptr<Selection::Selection> get_selection() const;

    WebIDL::ExceptionOr<String> cookie();
    WebIDL::ExceptionOr<void> set_cookie(StringView);
    bool is_cookie_averse() const;

    void set_cookie_version_index(Core::SharedVersionIndex cookie_version_index) { m_cookie_version_index = cookie_version_index; }
    void reset_cookie_version() { m_cookie_version = Core::INVALID_SHARED_VERSION; }

    String fg_color() const;
    void set_fg_color(String const&);

    String link_color() const;
    void set_link_color(String const&);

    String vlink_color() const;
    void set_vlink_color(String const&);

    String alink_color() const;
    void set_alink_color(String const&);

    String bg_color() const;
    void set_bg_color(String const&);

    String referrer() const;
    void set_referrer(String);

    void set_url(URL::URL const& url);
    URL::URL url() const { return m_url; }
    URL::URL fallback_base_url() const;
    URL::URL base_url() const;

    void update_base_element(Badge<HTML::HTMLBaseElement>);
    GC::Ptr<HTML::HTMLBaseElement> first_base_element_with_href_in_tree_order() const;
    GC::Ptr<HTML::HTMLBaseElement> first_base_element_with_target_in_tree_order() const;
    void respond_to_base_url_changes();

    String url_string() const { return m_url.to_string(); }
    String document_uri() const { return url_string(); }

    URL::Origin const& origin() const;
    void set_origin(URL::Origin const& origin);

    HTML::OpenerPolicy const& opener_policy() const { return m_opener_policy; }
    void set_opener_policy(HTML::OpenerPolicy policy) { m_opener_policy = move(policy); }

    Optional<URL::URL> encoding_parse_url(StringView) const;
    Optional<String> encoding_parse_and_serialize_url(StringView) const;

    CSS::StyleComputer& style_computer() { return *m_style_computer; }
    CSS::StyleComputer const& style_computer() const { return *m_style_computer; }

    CSS::FontComputer& font_computer() { return *m_font_computer; }
    CSS::FontComputer const& font_computer() const { return *m_font_computer; }

    CSS::StyleSheetList& style_sheets();
    CSS::StyleSheetList const& style_sheets() const;

    void for_each_active_css_style_sheet(Function<void(CSS::CSSStyleSheet&)> const& callback) const;

    CSS::StyleSheetList* style_sheets_for_bindings() { return &style_sheets(); }

    double ensure_element_shared_css_random_base_value(CSS::RandomCachingKey const&);

    Optional<String> get_style_sheet_source(CSS::StyleSheetIdentifier const&) const;

    virtual FlyString node_name() const override { return "#document"_fly_string; }

    void invalidate_style_for_elements_affected_by_pseudo_class_change(CSS::PseudoClass, auto& element_slot, Node& old_new_common_ancestor, auto node);

    void set_hovered_node(GC::Ptr<Node>);
    Node* hovered_node() { return m_hovered_node.ptr(); }
    Node const* hovered_node() const { return m_hovered_node.ptr(); }

    void set_inspected_node(GC::Ptr<Node>);
    GC::Ptr<Node const> inspected_node() const { return m_inspected_node; }

    void set_highlighted_node(GC::Ptr<Node>, Optional<CSS::PseudoElement>);
    GC::Ptr<Node const> highlighted_node() const { return m_highlighted_node; }
    GC::Ptr<Layout::Node> highlighted_layout_node();
    GC::Ptr<Layout::Node const> highlighted_layout_node() const { return const_cast<Document*>(this)->highlighted_layout_node(); }

    Element* document_element();
    Element const* document_element() const;

    // https://www.w3.org/TR/SVG2/struct.html#InterfaceDocumentExtensions
    GC::Ptr<SVG::SVGSVGElement> root_element();

    HTML::HTMLHtmlElement* html_element();
    HTML::HTMLHeadElement* head();
    GC::Ptr<HTML::HTMLTitleElement> title_element();

    StringView dir() const;
    void set_dir(String const&);

    HTML::HTMLElement* body();

    HTML::HTMLHtmlElement const* html_element() const
    {
        return const_cast<Document*>(this)->html_element();
    }

    HTML::HTMLHeadElement const* head() const
    {
        return const_cast<Document*>(this)->head();
    }

    GC::Ptr<HTML::HTMLTitleElement const> title_element() const
    {
        return const_cast<Document*>(this)->title_element();
    }

    HTML::HTMLElement const* body() const
    {
        return const_cast<Document*>(this)->body();
    }

    WebIDL::ExceptionOr<void> set_body(HTML::HTMLElement* new_body);

    Utf16String title() const;
    WebIDL::ExceptionOr<void> set_title(Utf16String const&);

    GC::Ptr<HTML::BrowsingContext> browsing_context() { return m_browsing_context; }
    GC::Ptr<HTML::BrowsingContext const> browsing_context() const { return m_browsing_context; }

    void set_browsing_context(GC::Ptr<HTML::BrowsingContext>);

    Page& page();
    Page const& page() const;

    Color background_color() const;
    Vector<CSS::BackgroundLayerData> const* background_layers() const;
    CSS::ImageRendering background_image_rendering() const;

    Optional<Color> normal_link_color() const;
    void set_normal_link_color(Color);

    Optional<Color> active_link_color() const;
    void set_active_link_color(Color);

    Optional<Color> visited_link_color() const;
    void set_visited_link_color(Color);

    Optional<Vector<String> const&> supported_color_schemes() const;
    void obtain_supported_color_schemes();

    void obtain_theme_color();

    void update_style();
    void update_style_if_needed_for_element(AbstractElement const&);
    [[nodiscard]] bool element_needs_style_update(AbstractElement const&) const;
    void update_layout(UpdateLayoutReason);
    [[nodiscard]] bool layout_is_up_to_date() const;
    void update_paint_and_hit_testing_properties_if_needed();
    void update_animated_style_if_needed();

    void invalidate_layout_tree(InvalidateLayoutTreeReason);
    void invalidate_stacking_context_tree();

    virtual bool is_child_allowed(Node const&) const override;

    Layout::Viewport const* layout_node() const;
    Layout::Viewport* layout_node();

    Layout::Viewport const* unsafe_layout_node() const;
    Layout::Viewport* unsafe_layout_node();

    Painting::ViewportPaintable const* paintable() const;
    Painting::ViewportPaintable* paintable();

    Painting::ViewportPaintable const* unsafe_paintable() const;
    Painting::ViewportPaintable* unsafe_paintable();

    GC::Ref<NodeList> get_elements_by_name(FlyString const&);

    GC::Ref<HTMLCollection> applets();
    GC::Ref<HTMLCollection> anchors();
    GC::Ref<HTMLCollection> images();
    GC::Ref<HTMLCollection> embeds();
    GC::Ref<HTMLCollection> plugins();
    GC::Ref<HTMLCollection> links();
    GC::Ref<HTMLCollection> forms();
    GC::Ref<HTMLCollection> scripts();
    GC::Ref<HTML::HTMLAllCollection> all();

    // https://drafts.csswg.org/css-font-loading/#font-source
    GC::Ref<CSS::FontFaceSet> fonts();

    void clear();
    void capture_events();
    void release_events();

    String const& source() const { return m_source; }
    void set_source(String source) { m_source = move(source); }

    HTML::EnvironmentSettingsObject& relevant_settings_object() const;

    WebIDL::ExceptionOr<GC::Ref<Element>> create_element(String const& local_name, Variant<String, ElementCreationOptions> const& options);
    WebIDL::ExceptionOr<GC::Ref<Element>> create_element_ns(Optional<FlyString> const& namespace_, String const& qualified_name, Variant<String, ElementCreationOptions> const& options);
    GC::Ref<DocumentFragment> create_document_fragment();
    GC::Ref<Text> create_text_node(Utf16String data);
    WebIDL::ExceptionOr<GC::Ref<CDATASection>> create_cdata_section(Utf16String data);
    GC::Ref<Comment> create_comment(Utf16String data);
    WebIDL::ExceptionOr<GC::Ref<ProcessingInstruction>> create_processing_instruction(String const& target, Utf16String data);

    WebIDL::ExceptionOr<GC::Ref<Attr>> create_attribute(String const& local_name);
    WebIDL::ExceptionOr<GC::Ref<Attr>> create_attribute_ns(Optional<FlyString> const& namespace_, String const& qualified_name);

    WebIDL::ExceptionOr<GC::Ref<Event>> create_event(StringView interface);
    GC::Ref<Range> create_range();

    void set_pending_parsing_blocking_script(HTML::HTMLScriptElement*);
    HTML::HTMLScriptElement* pending_parsing_blocking_script() { return m_pending_parsing_blocking_script.ptr(); }
    GC::Ref<HTML::HTMLScriptElement> take_pending_parsing_blocking_script(Badge<HTML::HTMLParser>);

    void add_script_to_execute_when_parsing_has_finished(Badge<HTML::HTMLScriptElement>, HTML::HTMLScriptElement&);
    Vector<GC::Root<HTML::HTMLScriptElement>> take_scripts_to_execute_when_parsing_has_finished(Badge<HTML::HTMLParser>);
    Vector<GC::Ref<HTML::HTMLScriptElement>>& scripts_to_execute_when_parsing_has_finished() { return m_scripts_to_execute_when_parsing_has_finished; }

    void add_script_to_execute_as_soon_as_possible(Badge<HTML::HTMLScriptElement>, HTML::HTMLScriptElement&);
    Vector<GC::Root<HTML::HTMLScriptElement>> take_scripts_to_execute_as_soon_as_possible(Badge<HTML::HTMLParser>);
    Vector<GC::Ref<HTML::HTMLScriptElement>>& scripts_to_execute_as_soon_as_possible() { return m_scripts_to_execute_as_soon_as_possible; }

    void add_script_to_execute_in_order_as_soon_as_possible(Badge<HTML::HTMLScriptElement>, HTML::HTMLScriptElement&);
    Vector<GC::Root<HTML::HTMLScriptElement>> take_scripts_to_execute_in_order_as_soon_as_possible(Badge<HTML::HTMLParser>);
    Vector<GC::Ref<HTML::HTMLScriptElement>>& scripts_to_execute_in_order_as_soon_as_possible() { return m_scripts_to_execute_in_order_as_soon_as_possible; }

    QuirksMode mode() const { return m_quirks_mode; }
    bool in_quirks_mode() const { return m_quirks_mode == QuirksMode::Yes; }
    bool in_limited_quirks_mode() const { return m_quirks_mode == QuirksMode::Limited; }
    void set_quirks_mode(QuirksMode mode) { m_quirks_mode = mode; }

    bool parser_cannot_change_the_mode() const { return m_parser_cannot_change_the_mode; }
    void set_parser_cannot_change_the_mode(bool parser_cannot_change_the_mode) { m_parser_cannot_change_the_mode = parser_cannot_change_the_mode; }

    Type document_type() const { return m_type; }
    void set_document_type(Type type) { m_type = type; }

    // https://dom.spec.whatwg.org/#html-document
    bool is_html_document() const { return m_type == Type::HTML; }

    // https://dom.spec.whatwg.org/#xml-document
    bool is_xml_document() const { return m_type == Type::XML; }

    WebIDL::ExceptionOr<GC::Ref<Node>> import_node(GC::Ref<Node> node, bool deep);
    void adopt_node(Node&);
    WebIDL::ExceptionOr<GC::Ref<Node>> adopt_node_binding(GC::Ref<Node>);

    DocumentType const* doctype() const;
    String const& compat_mode() const;

    void set_editable(bool editable) { m_editable = editable; }

    // https://html.spec.whatwg.org/multipage/interaction.html#focused-area-of-the-document
    GC::Ptr<Node> focused_area() { return m_focused_area; }
    GC::Ptr<Node const> focused_area() const { return m_focused_area; }
    void set_focused_area(GC::Ptr<Node>);

    HTML::FocusTrigger last_focus_trigger() const { return m_last_focus_trigger; }
    void set_last_focus_trigger(HTML::FocusTrigger trigger) { m_last_focus_trigger = trigger; }

    Element const* active_element() const;
    void set_active_element(GC::Ptr<Element>);

    Element const* target_element() const { return m_target_element.ptr(); }
    void set_target_element(GC::Ptr<Element>);

    void try_to_scroll_to_the_fragment();
    void scroll_to_the_fragment();
    void scroll_to_the_beginning_of_the_document();

    bool created_for_appropriate_template_contents() const { return m_created_for_appropriate_template_contents; }

    GC::Ref<Document> appropriate_template_contents_owner_document();

    StringView ready_state() const;
    HTML::DocumentReadyState readiness() const { return m_readiness; }
    void update_readiness(HTML::DocumentReadyState);

    String last_modified() const;

    [[nodiscard]] GC::Ptr<HTML::Window> window() const { return m_window; }

    void set_window(HTML::Window&);

    WebIDL::ExceptionOr<void> write(Vector<TrustedTypes::TrustedHTMLOrString> const& text);
    WebIDL::ExceptionOr<void> writeln(Vector<TrustedTypes::TrustedHTMLOrString> const& text);

    WebIDL::ExceptionOr<Document*> open(Optional<String> const& = {}, Optional<String> const& = {});
    WebIDL::ExceptionOr<GC::Ptr<HTML::WindowProxy>> open(StringView url, StringView name, StringView features);
    WebIDL::ExceptionOr<void> close();

    GC::Ptr<HTML::WindowProxy const> default_view() const;
    GC::Ptr<HTML::WindowProxy> default_view();

    String const& content_type() const { return m_content_type; }
    void set_content_type(String content_type) { m_content_type = move(content_type); }

    Optional<String> const& pragma_set_default_language() const { return m_pragma_set_default_language; }
    void set_pragma_set_default_language(String language) { m_pragma_set_default_language = move(language); }
    Optional<String> const& http_content_language() const { return m_http_content_language; }

    bool has_encoding() const { return m_encoding.has_value(); }
    Optional<String> const& encoding() const { return m_encoding; }
    String encoding_or_default() const { return m_encoding.value_or("UTF-8"_string); }
    void set_encoding(Optional<String> encoding) { m_encoding = move(encoding); }

    // NOTE: These are intended for the JS bindings
    String character_set() const { return encoding_or_default(); }
    String charset() const { return encoding_or_default(); }
    String input_encoding() const { return encoding_or_default(); }

    bool ready_for_post_load_tasks() const { return m_ready_for_post_load_tasks; }
    void set_ready_for_post_load_tasks(bool ready) { m_ready_for_post_load_tasks = ready; }

    void completely_finish_loading();

    DOMImplementation* implementation();

    GC::Ptr<HTML::HTMLScriptElement> current_script() const { return m_current_script.ptr(); }
    void set_current_script(Badge<HTML::HTMLScriptElement>, GC::Ptr<HTML::HTMLScriptElement> script) { m_current_script = move(script); }

    u32 ignore_destructive_writes_counter() const { return m_ignore_destructive_writes_counter; }
    void increment_ignore_destructive_writes_counter() { m_ignore_destructive_writes_counter++; }
    void decrement_ignore_destructive_writes_counter() { m_ignore_destructive_writes_counter--; }

    virtual EventTarget* get_parent(Event const&) override;

    String dump_dom_tree_as_json() const;

    [[nodiscard]] bool has_a_style_sheet_that_is_blocking_scripts() const;
    [[nodiscard]] bool has_no_style_sheet_that_is_blocking_scripts() const;

    bool is_fully_active() const;
    bool is_active() const;

    [[nodiscard]] bool allow_declarative_shadow_roots() const;
    void set_allow_declarative_shadow_roots(bool);

    GC::Ref<HTML::History> history();
    GC::Ref<HTML::History> history() const;

    [[nodiscard]] GC::Ptr<HTML::Location> location();

    bool anything_is_delaying_the_load_event() const;
    void increment_number_of_things_delaying_the_load_event(Badge<DocumentLoadEventDelayer>);
    void decrement_number_of_things_delaying_the_load_event(Badge<DocumentLoadEventDelayer>);

    void add_pending_css_import_rule(Badge<CSS::CSSImportRule>, GC::Ref<CSS::CSSImportRule>);
    void remove_pending_css_import_rule(Badge<CSS::CSSImportRule>, GC::Ref<CSS::CSSImportRule>);

    bool page_showing() const { return m_page_showing; }
    void set_page_showing(bool);

    bool hidden() const;
    StringView visibility_state() const;
    HTML::VisibilityState visibility_state_value() const { return m_visibility_state; }

    // https://html.spec.whatwg.org/multipage/interaction.html#update-the-visibility-state
    void update_the_visibility_state(HTML::VisibilityState);

    void run_the_resize_steps();
    void run_the_scroll_steps();

    void evaluate_media_queries_and_report_changes();
    void set_needs_media_query_evaluation() { m_needs_media_query_evaluation = true; }
    void add_media_query_list(GC::Ref<CSS::MediaQueryList>);

    GC::Ref<CSS::VisualViewport> visual_viewport();
    [[nodiscard]] CSSPixelRect viewport_rect() const;

    void register_viewport_client(ViewportClient&);
    void unregister_viewport_client(ViewportClient&);
    void inform_all_viewport_clients_about_the_current_viewport_rect();

    bool has_focus_for_bindings() const;
    bool has_focus() const;

    bool allow_focus() const;

    void set_parser(Badge<HTML::HTMLParser>, HTML::HTMLParser&);
    void detach_parser(Badge<HTML::HTMLParser>);

    [[nodiscard]] bool is_temporary_document_for_fragment_parsing() const { return m_temporary_document_for_fragment_parsing == TemporaryDocumentForFragmentParsing::Yes; }

    static bool is_valid_name(String const&);

    GC::Ref<NodeIterator> create_node_iterator(Node& root, unsigned what_to_show, GC::Ptr<NodeFilter>);
    GC::Ref<TreeWalker> create_tree_walker(Node& root, unsigned what_to_show, GC::Ptr<NodeFilter>);

    void register_node_iterator(Badge<NodeIterator>, NodeIterator&);
    void unregister_node_iterator(Badge<NodeIterator>, NodeIterator&);

    void register_document_observer(Badge<DocumentObserver>, DocumentObserver&);
    void unregister_document_observer(Badge<DocumentObserver>, DocumentObserver&);

    template<typename Callback>
    void for_each_node_iterator(Callback callback)
    {
        for (auto& node_iterator : m_node_iterators)
            callback(*node_iterator);
    }

    bool needs_full_style_update() const { return m_needs_full_style_update; }
    void set_needs_full_style_update(bool b) { m_needs_full_style_update = b; }

    [[nodiscard]] bool needs_full_layout_tree_update() const { return m_needs_full_layout_tree_update; }
    void set_needs_full_layout_tree_update(bool b) { m_needs_full_layout_tree_update = b; }

    void mark_svg_root_as_needing_relayout(Layout::SVGSVGBox&);

    void set_needs_to_refresh_scroll_state(bool b);

    bool has_active_favicon() const { return m_active_favicon; }
    void check_favicon_after_loading_link_resource();

    GC::Ptr<HTML::CustomElementDefinition> lookup_custom_element_definition(Optional<FlyString> const& namespace_, FlyString const& local_name, Optional<String> const& is) const;

    void increment_throw_on_dynamic_markup_insertion_counter(Badge<HTML::HTMLParser>);
    void decrement_throw_on_dynamic_markup_insertion_counter(Badge<HTML::HTMLParser>);

    // https://html.spec.whatwg.org/multipage/dom.html#is-initial-about:blank
    bool is_initial_about_blank() const { return m_is_initial_about_blank; }
    void set_is_initial_about_blank(bool b) { m_is_initial_about_blank = b; }

    // https://html.spec.whatwg.org/multipage/dom.html#concept-document-about-base-url
    Optional<URL::URL> about_base_url() const { return m_about_base_url; }
    void set_about_base_url(Optional<URL::URL> url) { m_about_base_url = url; }

    String domain() const;
    WebIDL::ExceptionOr<void> set_domain(String const&);

    struct PendingScrollEvent {
        GC::Ref<EventTarget> event_target;
        FlyString event_type;
        bool operator==(PendingScrollEvent const&) const = default;
    };
    Vector<PendingScrollEvent>& pending_scroll_events() { return m_pending_scroll_events; }

    // https://html.spec.whatwg.org/multipage/document-lifecycle.html#completely-loaded
    bool is_completely_loaded() const;

    // https://html.spec.whatwg.org/multipage/dom.html#concept-document-navigation-id
    Optional<String> navigation_id() const;
    void set_navigation_id(Optional<String>);

    // https://html.spec.whatwg.org/multipage/origin.html#active-sandboxing-flag-set
    HTML::SandboxingFlagSet active_sandboxing_flag_set() const;
    void set_active_sandboxing_flag_set(HTML::SandboxingFlagSet);

    // https://html.spec.whatwg.org/multipage/dom.html#concept-document-policy-container
    GC::Ref<HTML::PolicyContainer> policy_container() const;
    void set_policy_container(GC::Ref<HTML::PolicyContainer>);

    Vector<GC::Root<HTML::Navigable>> descendant_navigables();
    Vector<GC::Root<HTML::Navigable>> const descendant_navigables() const;
    Vector<GC::Root<HTML::Navigable>> inclusive_descendant_navigables();
    Vector<GC::Root<HTML::Navigable>> ancestor_navigables();
    Vector<GC::Root<HTML::Navigable>> const ancestor_navigables() const;
    Vector<GC::Root<HTML::Navigable>> inclusive_ancestor_navigables();
    Vector<GC::Root<HTML::Navigable>> document_tree_child_navigables();

    [[nodiscard]] bool has_been_destroyed() const { return m_has_been_destroyed; }

    [[nodiscard]] bool has_been_browsing_context_associated() const { return m_has_been_browsing_context_associated; }

    // https://html.spec.whatwg.org/multipage/document-lifecycle.html#destroy-a-document
    void destroy();
    // https://html.spec.whatwg.org/multipage/document-lifecycle.html#destroy-a-document-and-its-descendants
    void destroy_a_document_and_its_descendants(GC::Ptr<GC::Function<void()>> after_all_destruction = {});

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#abort-a-document
    void abort();
    // https://html.spec.whatwg.org/multipage/document-lifecycle.html#abort-a-document-and-its-descendants
    void abort_a_document_and_its_descendants();

    // https://html.spec.whatwg.org/multipage/document-lifecycle.html#unload-a-document
    void unload(GC::Ptr<Document> new_document = nullptr);
    // https://html.spec.whatwg.org/multipage/document-lifecycle.html#unload-a-document-and-its-descendants
    void unload_a_document_and_its_descendants(GC::Ptr<Document> new_document, GC::Ptr<GC::Function<void()>> after_all_unloads = {});

    // https://html.spec.whatwg.org/multipage/dom.html#active-parser
    GC::Ptr<HTML::HTMLParser> active_parser();

    // https://html.spec.whatwg.org/multipage/dom.html#load-timing-info
    DocumentLoadTimingInfo& load_timing_info() { return m_load_timing_info; }
    DocumentLoadTimingInfo const& load_timing_info() const { return m_load_timing_info; }
    void set_load_timing_info(DocumentLoadTimingInfo const& load_timing_info) { m_load_timing_info = load_timing_info; }

    // https://html.spec.whatwg.org/multipage/dom.html#previous-document-unload-timing
    DocumentUnloadTimingInfo& previous_document_unload_timing() { return m_previous_document_unload_timing; }
    DocumentUnloadTimingInfo const& previous_document_unload_timing() const { return m_previous_document_unload_timing; }
    void set_previous_document_unload_timing(DocumentUnloadTimingInfo const& previous_document_unload_timing) { m_previous_document_unload_timing = previous_document_unload_timing; }

    // https://w3c.github.io/editing/docs/execCommand/
    WebIDL::ExceptionOr<bool> exec_command(FlyString const& command, bool show_ui, Utf16String const& value);
    WebIDL::ExceptionOr<bool> query_command_enabled(FlyString const& command);
    WebIDL::ExceptionOr<bool> query_command_indeterm(FlyString const& command);
    WebIDL::ExceptionOr<bool> query_command_state(FlyString const& command);
    WebIDL::ExceptionOr<bool> query_command_supported(FlyString const& command);
    WebIDL::ExceptionOr<String> query_command_value(FlyString const& command);

    WebIDL::ExceptionOr<GC::Ref<XPath::XPathExpression>> create_expression(String const& expression, GC::Ptr<XPath::XPathNSResolver> resolver = nullptr);
    WebIDL::ExceptionOr<GC::Ref<XPath::XPathResult>> evaluate(String const& expression, DOM::Node const& context_node, GC::Ptr<XPath::XPathNSResolver> resolver = nullptr, WebIDL::UnsignedShort type = 0, GC::Ptr<XPath::XPathResult> result = nullptr);
    GC::Ref<DOM::Node> create_ns_resolver(GC::Ref<DOM::Node> node_resolver); // legacy

    // https://w3c.github.io/selection-api/#dfn-has-scheduled-selectionchange-event
    bool has_scheduled_selectionchange_event() const { return m_has_scheduled_selectionchange_event; }
    void set_scheduled_selectionchange_event(bool value) { m_has_scheduled_selectionchange_event = value; }

    bool is_allowed_to_use_feature(PolicyControlledFeature) const;

    void did_stop_being_active_document_in_navigable();

    String dump_accessibility_tree_as_json();

    void make_active();

    void set_salvageable(bool value) { m_salvageable = value; }

    void make_unsalvageable(String reason);

    HTML::ListOfAvailableImages& list_of_available_images();
    HTML::ListOfAvailableImages const& list_of_available_images() const;

    void register_intersection_observer(Badge<IntersectionObserver::IntersectionObserver>, IntersectionObserver::IntersectionObserver&);
    void unregister_intersection_observer(Badge<IntersectionObserver::IntersectionObserver>, IntersectionObserver::IntersectionObserver&);

    void register_resize_observer(Badge<ResizeObserver::ResizeObserver>, ResizeObserver::ResizeObserver&);
    void unregister_resize_observer(Badge<ResizeObserver::ResizeObserver>, ResizeObserver::ResizeObserver&);

    void run_the_update_intersection_observations_steps(HighResolutionTime::DOMHighResTimeStamp time);

    void start_intersection_observing_a_lazy_loading_element(Element&);
    void stop_intersection_observing_a_lazy_loading_element(Element&);

    void shared_declarative_refresh_steps(StringView input, GC::Ptr<HTML::HTMLMetaElement const> meta_element = nullptr);

    struct TopOfTheDocument { };
    using IndicatedPart = Variant<Element*, TopOfTheDocument>;
    IndicatedPart determine_the_indicated_part() const;

    u32 unload_counter() const { return m_unload_counter; }

    GC::Ref<HTML::SourceSnapshotParams> snapshot_source_snapshot_params() const;

    void update_for_history_step_application(GC::Ref<HTML::SessionHistoryEntry>, bool do_not_reactivate, size_t script_history_length, size_t script_history_index, Optional<Bindings::NavigationType> navigation_type, Optional<Vector<GC::Ref<HTML::SessionHistoryEntry>>> entries_for_navigation_api = {}, GC::Ptr<HTML::SessionHistoryEntry> previous_entry_for_activation = {}, bool update_navigation_api = true);

    HashMap<URL::URL, GC::Ptr<HTML::SharedResourceRequest>>& shared_resource_requests();

    void restore_the_history_object_state(GC::Ref<HTML::SessionHistoryEntry> entry);

    GC::Ref<Animations::DocumentTimeline> timeline();
    auto const& last_animation_frame_timestamp() const { return m_last_animation_frame_timestamp; }

    void associate_with_timeline(GC::Ref<Animations::AnimationTimeline>);
    void disassociate_with_timeline(GC::Ref<Animations::AnimationTimeline>);

    struct PendingAnimationEvent {
        GC::Ref<DOM::Event> event;
        GC::Ref<Animations::Animation> animation;
        GC::Ref<DOM::EventTarget> target;
        Optional<double> scheduled_event_time;
    };
    void append_pending_animation_event(PendingAnimationEvent const&);
    void update_animations_and_send_events(double timestamp);
    void remove_replaced_animations();

    WebIDL::ExceptionOr<Vector<GC::Ref<Animations::Animation>>> get_animations();

    bool ready_to_run_scripts() const { return m_ready_to_run_scripts; }
    void set_ready_to_run_scripts() { m_ready_to_run_scripts = true; }

    GC::Ptr<HTML::SessionHistoryEntry> latest_entry() const { return m_latest_entry; }
    void set_latest_entry(GC::Ptr<HTML::SessionHistoryEntry> e) { m_latest_entry = e; }

    void element_id_changed(Badge<DOM::Element>, GC::Ref<DOM::Element> element, Optional<FlyString> old_id);
    void element_with_id_was_added(Badge<DOM::Element>, GC::Ref<DOM::Element> element);
    void element_with_id_was_removed(Badge<DOM::Element>, GC::Ref<DOM::Element> element);
    void element_name_changed(Badge<DOM::Element>, GC::Ref<DOM::Element> element);
    void element_with_name_was_added(Badge<DOM::Element>, GC::Ref<DOM::Element> element);
    void element_with_name_was_removed(Badge<DOM::Element>, GC::Ref<DOM::Element> element);

    void add_form_associated_element_with_form_attribute(HTML::FormAssociatedElement&);
    void remove_form_associated_element_with_form_attribute(HTML::FormAssociatedElement&);

    bool design_mode_enabled_state() const { return m_design_mode_enabled; }
    void set_design_mode_enabled_state(bool);
    String design_mode() const;
    WebIDL::ExceptionOr<void> set_design_mode(String const&);

    Element const* element_from_point(double x, double y);
    GC::RootVector<GC::Ref<Element>> elements_from_point(double x, double y);
    GC::Ptr<Element const> scrolling_element() const;

    void set_needs_to_resolve_paint_only_properties() { m_needs_to_resolve_paint_only_properties = true; }
    void set_needs_animated_style_update() { m_needs_animated_style_update = true; }

    void set_needs_invalidation_of_elements_affected_by_has() { m_needs_invalidation_of_elements_affected_by_has = true; }

    void set_needs_accumulated_visual_contexts_update(bool value) { m_needs_accumulated_visual_contexts_update = value; }
    bool needs_accumulated_visual_contexts_update() const { return m_needs_accumulated_visual_contexts_update; }

    virtual JS::Value named_item_value(FlyString const& name) const override;
    virtual Vector<FlyString> supported_property_names() const override;
    Vector<GC::Ref<DOM::Element>> const& potentially_named_elements() const { return m_potentially_named_elements; }

    void gather_active_observations_at_depth(size_t depth);
    [[nodiscard]] size_t broadcast_active_resize_observations();
    [[nodiscard]] bool has_active_resize_observations();
    [[nodiscard]] bool has_skipped_resize_observations();

    GC::Ref<WebIDL::ObservableArray> adopted_style_sheets() const;
    WebIDL::ExceptionOr<void> set_adopted_style_sheets(JS::Value);

    void register_shadow_root(Badge<DOM::ShadowRoot>, DOM::ShadowRoot&);
    void unregister_shadow_root(Badge<DOM::ShadowRoot>, DOM::ShadowRoot&);
    template<typename Callback>
    void for_each_shadow_root(Callback&& callback)
    {
        for (auto& shadow_root : m_shadow_roots)
            callback(shadow_root);
    }

    template<typename Callback>
    void for_each_shadow_root(Callback&& callback) const
    {
        for (auto& shadow_root : m_shadow_roots)
            callback(const_cast<ShadowRoot&>(shadow_root));
    }

    void add_an_element_to_the_top_layer(GC::Ref<Element>);
    void request_an_element_to_be_remove_from_the_top_layer(GC::Ref<Element>);
    void remove_an_element_from_the_top_layer_immediately(GC::Ref<Element>);
    void process_top_layer_removals();

    OrderedHashTable<GC::Ref<Element>> const& top_layer_elements() const { return m_top_layer_elements; }

    // AD-HOC: These lists are managed dynamically instead of being generated as needed.
    // Spec issue: https://github.com/whatwg/html/issues/11007
    Vector<GC::Ref<HTML::HTMLElement>>& showing_auto_popover_list() { return m_showing_auto_popover_list; }
    Vector<GC::Ref<HTML::HTMLElement>>& showing_hint_popover_list() { return m_showing_hint_popover_list; }
    Vector<GC::Ref<HTML::HTMLElement>> const& showing_auto_popover_list() const { return m_showing_auto_popover_list; }
    Vector<GC::Ref<HTML::HTMLElement>> const& showing_hint_popover_list() const { return m_showing_hint_popover_list; }

    GC::Ptr<HTML::HTMLElement> topmost_auto_or_hint_popover();

    void set_popover_pointerdown_target(GC::Ptr<HTML::HTMLElement> target) { m_popover_pointerdown_target = target; }
    GC::Ptr<HTML::HTMLElement> popover_pointerdown_target() { return m_popover_pointerdown_target; }

    Vector<GC::Ref<HTML::HTMLDialogElement>>& open_dialogs_list() { return m_open_dialogs_list; }

    void set_dialog_pointerdown_target(GC::Ptr<HTML::HTMLDialogElement> target) { m_dialog_pointerdown_target = target; }
    GC::Ptr<HTML::HTMLDialogElement> dialog_pointerdown_target() { return m_dialog_pointerdown_target; }

    size_t transition_generation() const { return m_transition_generation; }

    // Does document represent an embedded svg img
    [[nodiscard]] bool is_decoded_svg() const;

    Vector<GC::Root<Range>> find_matching_text(String const&, CaseSensitivity);

    void parse_html_from_a_string(StringView);
    static WebIDL::ExceptionOr<GC::Root<DOM::Document>> parse_html_unsafe(JS::VM&, TrustedTypes::TrustedHTMLOrString const&);

    void set_console_client(GC::Ptr<JS::ConsoleClient> console_client) { m_console_client = console_client; }
    GC::Ptr<JS::ConsoleClient> console_client() const { return m_console_client; }

    InputEventsTarget* active_input_events_target(DOM::Node const* for_node = nullptr);
    GC::Ptr<DOM::Position> cursor_position() const;

    bool cursor_blink_state() const { return m_cursor_blink_state; }

    // Cached pointer to the last known node navigable.
    // If this document is currently the "active document" of the cached navigable, the cache is still valid.
    GC::Ptr<HTML::Navigable> cached_navigable();
    void set_cached_navigable(GC::Ptr<HTML::Navigable>);

    void set_needs_display(InvalidateDisplayList = InvalidateDisplayList::Yes);
    void set_needs_display(CSSPixelRect const&, InvalidateDisplayList = InvalidateDisplayList::Yes);

    RefPtr<Painting::DisplayList> cached_display_list() const;
    RefPtr<Painting::DisplayList> record_display_list(HTML::PaintConfig);

    void invalidate_display_list();

    Unicode::Segmenter& grapheme_segmenter() const;
    Unicode::Segmenter& line_segmenter() const;
    Unicode::Segmenter& word_segmenter() const;

    struct StepsToFireBeforeunloadResult {
        bool unload_prompt_shown { false };
        bool unload_prompt_canceled { false };
    };
    StepsToFireBeforeunloadResult steps_to_fire_beforeunload(bool unload_prompt_shown);

    [[nodiscard]] WebIDL::CallbackType* onreadystatechange();
    void set_onreadystatechange(WebIDL::CallbackType*);

    [[nodiscard]] WebIDL::CallbackType* onvisibilitychange();
    void set_onvisibilitychange(WebIDL::CallbackType*);

    // https://fullscreen.spec.whatwg.org/#api
    [[nodiscard]] WebIDL::CallbackType* onfullscreenchange();
    void set_onfullscreenchange(WebIDL::CallbackType*);
    [[nodiscard]] WebIDL::CallbackType* onfullscreenerror();
    void set_onfullscreenerror(WebIDL::CallbackType*);

    // https://drafts.csswg.org/css-view-transitions-1/#dom-document-startviewtransition
    GC::Ptr<ViewTransition::ViewTransition> start_view_transition(GC::Ptr<WebIDL::CallbackType> update_callback);
    // https://drafts.csswg.org/css-view-transitions-1/#perform-pending-transition-operations
    void perform_pending_transition_operations();
    // https://drafts.csswg.org/css-view-transitions-1/#flush-the-update-callback-queue
    void flush_the_update_callback_queue();
    // https://drafts.csswg.org/css-view-transitions-1/#view-transition-page-visibility-change-steps
    void view_transition_page_visibility_change_steps();

    GC::Ptr<ViewTransition::ViewTransition> active_view_transition() const { return m_active_view_transition; }
    void set_active_view_transition(GC::Ptr<ViewTransition::ViewTransition> view_transition) { m_active_view_transition = view_transition; }
    bool rendering_suppression_for_view_transitions() const { return m_rendering_suppression_for_view_transitions; }
    void set_rendering_suppression_for_view_transitions(bool value) { m_rendering_suppression_for_view_transitions = value; }
    GC::Ptr<CSS::CSSStyleSheet> dynamic_view_transition_style_sheet() const { return m_dynamic_view_transition_style_sheet; }
    void set_show_view_transition_tree(bool value) { m_show_view_transition_tree = value; }
    Vector<GC::Ptr<ViewTransition::ViewTransition>>& update_callback_queue() { return m_update_callback_queue; }

    void reset_cursor_blink_cycle();

    GC::Ref<EditingHostManager> editing_host_manager() const { return *m_editing_host_manager; }

    // https://w3c.github.io/editing/docs/execCommand/#default-single-line-container-name
    FlyString const& default_single_line_container_name() const { return m_default_single_line_container_name; }
    void set_default_single_line_container_name(FlyString const& name) { m_default_single_line_container_name = name; }

    // https://w3c.github.io/editing/docs/execCommand/#css-styling-flag
    bool css_styling_flag() const { return m_css_styling_flag; }
    void set_css_styling_flag(bool flag) { m_css_styling_flag = flag; }

    // https://w3c.github.io/editing/docs/execCommand/#state-override
    Optional<bool> command_state_override(FlyString const& command) const { return m_command_state_override.get(command); }
    void set_command_state_override(FlyString const& command, bool state) { m_command_state_override.set(command, state); }
    void clear_command_state_override(FlyString const& command) { m_command_state_override.remove(command); }
    void reset_command_state_overrides() { m_command_state_override.clear(); }

    // https://w3c.github.io/editing/docs/execCommand/#value-override
    Optional<Utf16String const&> command_value_override(FlyString const& command) const { return m_command_value_override.get(command); }
    void set_command_value_override(FlyString const& command, Utf16String const& value);
    void clear_command_value_override(FlyString const& command);
    void reset_command_value_overrides() { m_command_value_override.clear(); }

    GC::Ptr<DOM::Document> container_document() const;

    GC::Ptr<HTML::Storage> session_storage_holder() { return m_session_storage_holder; }
    void set_session_storage_holder(GC::Ptr<HTML::Storage> storage) { m_session_storage_holder = storage; }

    GC::Ptr<HTML::Storage> local_storage_holder() { return m_local_storage_holder; }
    void set_local_storage_holder(GC::Ptr<HTML::Storage> storage) { m_local_storage_holder = storage; }

    // https:// html.spec.whatwg.org/multipage/dom.html#render-blocked
    [[nodiscard]] bool is_render_blocked() const;
    // https://html.spec.whatwg.org/multipage/dom.html#allows-adding-render-blocking-elements
    [[nodiscard]] bool allows_adding_render_blocking_elements() const;

    [[nodiscard]] bool is_render_blocking_element(GC::Ref<Element>) const;

    void add_render_blocking_element(GC::Ref<Element>);
    void remove_render_blocking_element(GC::Ref<Element>);

    ElementByIdMap& element_by_id() const;

    // https://fullscreen.spec.whatwg.org/#run-the-fullscreen-steps
    void run_fullscreen_steps();
    void append_pending_fullscreen_change(PendingFullscreenEvent::Type type, GC::Ref<Element> element);

    void fullscreen_element_within_doc(GC::Ref<Element> element);
    GC::Ptr<Element> fullscreen_element() const;
    GC::Ptr<Element> fullscreen_element_for_bindings() const;

    bool fullscreen() const;
    bool fullscreen_enabled() const;

    void fully_exit_fullscreen();
    GC::Ref<WebIDL::Promise> exit_fullscreen();

    void unfullscreen_element(GC::Ref<Element> element);

    auto& script_blocking_style_sheet_set() { return m_script_blocking_style_sheet_set; }
    auto const& script_blocking_style_sheet_set() const { return m_script_blocking_style_sheet_set; }

    String dump_display_list();
    String dump_stacking_context_tree();

    StyleInvalidator& style_invalidator() { return m_style_invalidator; }

    Optional<Vector<CSS::Parser::ComponentValue>> environment_variable_value(CSS::EnvironmentVariable, Span<i64> indices = {}) const;

    // https://www.w3.org/TR/css-properties-values-api-1/#dom-window-registeredpropertyset-slot
    HashMap<FlyString, CSS::CustomPropertyRegistration>& registered_property_set();
    Optional<CSS::CustomPropertyRegistration const&> get_registered_custom_property(FlyString const& name) const;
    NonnullRefPtr<CSS::StyleValue const> custom_property_initial_value(FlyString const& name) const;

    HashMap<FlyString, NonnullRefPtr<CSS::CounterStyle const>> const& registered_counter_styles() const { return m_registered_counter_styles; }

    CSS::StyleScope const& style_scope() const { return m_style_scope; }
    CSS::StyleScope& style_scope() { return m_style_scope; }

    void exit_pointer_lock();

protected:
    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    Document(JS::Realm&, URL::URL const&, TemporaryDocumentForFragmentParsing = TemporaryDocumentForFragmentParsing::No);

private:
    // ^JS::Object
    virtual bool is_dom_document() const final { return true; }

    // ^HTML::GlobalEventHandlers
    virtual GC::Ptr<EventTarget> global_event_handlers_to_event_target(FlyString const&) final { return *this; }

    virtual void finalize() override final;

    void invalidate_style_of_elements_affected_by_has();

    void tear_down_layout_tree();

    void update_active_element();

    void run_unloading_cleanup_steps();

    void evaluate_media_rules();

    bool is_simple_fullscreen_document() const;
    GC::Ref<GC::HeapVector<GC::Ref<Document>>> collect_documents_to_unfullscreen();

    enum class AddLineFeed {
        Yes,
        No,
    };
    WebIDL::ExceptionOr<void> run_the_document_write_steps(Vector<TrustedTypes::TrustedHTMLOrString> const& text, AddLineFeed line_feed, TrustedTypes::InjectionSink sink);

    void queue_intersection_observer_task();
    void queue_an_intersection_observer_entry(IntersectionObserver::IntersectionObserver&, HighResolutionTime::DOMHighResTimeStamp time, GC::Ref<Geometry::DOMRectReadOnly> root_bounds, GC::Ref<Geometry::DOMRectReadOnly> bounding_client_rect, GC::Ref<Geometry::DOMRectReadOnly> intersection_rect, bool is_intersecting, double intersection_ratio, GC::Ref<Element> target);

    Element* find_a_potential_indicated_element(FlyString const& fragment) const;

    void dispatch_events_for_transition(GC::Ref<CSS::CSSTransition>);
    void dispatch_events_for_animation_if_necessary(GC::Ref<Animations::Animation>);

    template<typename GetNotifier, typename... Args>
    void notify_each_document_observer(GetNotifier&& get_notifier, Args&&... args)
    {
        ScopeGuard guard { [&]() { m_document_observers_being_notified.clear_with_capacity(); } };
        m_document_observers_being_notified.ensure_capacity(m_document_observers.size());

        for (auto observer : m_document_observers)
            m_document_observers_being_notified.unchecked_append(observer);

        for (auto document_observer : m_document_observers_being_notified) {
            if (auto notifier = get_notifier(*document_observer))
                notifier->function()(forward<Args>(args)...);
        }
    }

    void run_csp_initialization() const;

    void build_registered_properties_cache();
    void build_counter_style_cache();

    void ensure_cookie_version_index(URL::URL const& new_url, URL::URL const& old_url = {});

    void unfullscreen();

    GC::Ref<Page> m_page;
    GC::Ptr<CSS::StyleComputer> m_style_computer;
    GC::Ptr<CSS::FontComputer> m_font_computer;
    GC::Ptr<CSS::StyleSheetList> m_style_sheets;
    GC::Ptr<Node> m_active_favicon;
    GC::Ptr<HTML::BrowsingContext> m_browsing_context;
    URL::URL m_url;
    mutable OwnPtr<ElementByIdMap> m_element_by_id;

    GC::Ptr<HTML::Window> m_window;

    GC::Ptr<Layout::Viewport> m_layout_root;

    GC::Ptr<Node> m_hovered_node;
    GC::Ptr<Node> m_inspected_node;
    GC::Ptr<Node> m_highlighted_node;
    Optional<CSS::PseudoElement> m_highlighted_pseudo_element;

    Optional<Color> m_normal_link_color;
    Optional<Color> m_active_link_color;
    Optional<Color> m_visited_link_color;

    Optional<Vector<String>> m_supported_color_schemes;

    GC::Ptr<HTML::HTMLParser> m_parser;
    bool m_active_parser_was_aborted { false };

    bool m_has_been_destroyed { false };
    bool m_has_fired_document_became_inactive { false };

    bool m_has_been_browsing_context_associated { false };

    String m_source;

    GC::Ptr<HTML::HTMLScriptElement> m_pending_parsing_blocking_script;

    Vector<GC::Ref<HTML::HTMLScriptElement>> m_scripts_to_execute_when_parsing_has_finished;

    // https://html.spec.whatwg.org/multipage/scripting.html#list-of-scripts-that-will-execute-in-order-as-soon-as-possible
    Vector<GC::Ref<HTML::HTMLScriptElement>> m_scripts_to_execute_in_order_as_soon_as_possible;

    // https://html.spec.whatwg.org/multipage/scripting.html#set-of-scripts-that-will-execute-as-soon-as-possible
    Vector<GC::Ref<HTML::HTMLScriptElement>> m_scripts_to_execute_as_soon_as_possible;

    QuirksMode m_quirks_mode { QuirksMode::No };

    bool m_parser_cannot_change_the_mode { false };

    // https://dom.spec.whatwg.org/#concept-document-type
    Type m_type { Type::XML };

    bool m_editable { false };

    // https://html.spec.whatwg.org/multipage/interaction.html#focused-area-of-the-document
    GC::Ptr<Node> m_focused_area;

    HTML::FocusTrigger m_last_focus_trigger { HTML::FocusTrigger::Other };

    GC::Ptr<Element> m_active_element;
    GC::Ptr<Element> m_target_element;

    bool m_created_for_appropriate_template_contents { false };
    GC::Ptr<Document> m_associated_inert_template_document;
    GC::Ptr<Document> m_appropriate_template_contents_owner_document;

    // https://html.spec.whatwg.org/multipage/dom.html#current-document-readiness
    // Each Document has a current document readiness, a string, initially "complete".
    // Spec Note: For Document objects created via the create and initialize a Document object algorithm, this will be
    //            immediately reset to "loading" before any script can observe the value of document.readyState.
    //            This default applies to other cases such as initial about:blank Documents or Documents without a
    //            browsing context.
    HTML::DocumentReadyState m_readiness { HTML::DocumentReadyState::Complete };
    String m_content_type { "application/xml"_string };
    Optional<String> m_pragma_set_default_language;
    Optional<String> m_http_content_language;
    Optional<String> m_encoding;

    bool m_ready_for_post_load_tasks { false };

    GC::Ptr<DOMImplementation> m_implementation;
    GC::Ptr<HTML::HTMLScriptElement> m_current_script;

    bool m_should_invalidate_styles_on_attribute_changes { true };

    u32 m_ignore_destructive_writes_counter { 0 };

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#unload-counter
    u32 m_unload_counter { 0 };

    // https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#throw-on-dynamic-markup-insertion-counter
    u32 m_throw_on_dynamic_markup_insertion_counter { 0 };

    // https://html.spec.whatwg.org/multipage/semantics.html#script-blocking-style-sheet-set
    HashTable<GC::Ref<DOM::Element>> m_script_blocking_style_sheet_set;

    HashTable<GC::Ref<CSS::CSSImportRule>> m_pending_css_import_rules;

    GC::Ptr<HTML::History> m_history;

    size_t m_number_of_things_delaying_the_load_event { 0 };

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#concept-document-salvageable
    bool m_salvageable { true };

    // https://html.spec.whatwg.org/multipage/document-lifecycle.html#page-showing
    bool m_page_showing { false };

    // Used by run_the_resize_steps().
    Optional<Gfx::IntSize> m_last_viewport_size;
    struct VisualViewportState {
        double scale { 1.0 };
        CSSPixelSize size;

        bool operator==(VisualViewportState const& other) const = default;
    };
    Optional<VisualViewportState> m_last_visual_viewport_state;

    HashTable<ViewportClient*> m_viewport_clients;

    // https://drafts.csswg.org/cssom-view-1/#document-pending-scroll-events
    // Each Document has an associated list of pending scroll events, which stores pairs of (EventTarget, DOMString), initially empty.
    Vector<PendingScrollEvent> m_pending_scroll_events;

    // Used by evaluate_media_queries_and_report_changes().
    bool m_needs_media_query_evaluation { false };
    Vector<GC::Weak<CSS::MediaQueryList>> m_media_query_lists;

    bool m_needs_full_style_update { false };
    bool m_needs_full_layout_tree_update { false };

    bool m_is_running_update_layout { false };

    HashTable<GC::Ref<Layout::SVGSVGBox>> m_svg_roots_needing_relayout;

    bool m_needs_animated_style_update { false };

    HashTable<GC::Ptr<NodeIterator>> m_node_iterators;

    // Document should not visit DocumentObserver to avoid leaks.
    // It's responsibility of object that requires DocumentObserver to keep it alive.
    HashTable<GC::RawRef<DocumentObserver>> m_document_observers;
    Vector<GC::Ref<DocumentObserver>> m_document_observers_being_notified;

    // https://html.spec.whatwg.org/multipage/dom.html#is-initial-about:blank
    bool m_is_initial_about_blank { false };

    // https://html.spec.whatwg.org/multipage/dom.html#concept-document-about-base-url
    Optional<URL::URL> m_about_base_url;

    // https://html.spec.whatwg.org/multipage/dom.html#concept-document-coop
    HTML::OpenerPolicy m_opener_policy;

    // https://html.spec.whatwg.org/multipage/dom.html#the-document's-referrer
    String m_referrer;

    // https://dom.spec.whatwg.org/#concept-document-origin
    URL::Origin m_origin { URL::Origin::create_opaque() };

    GC::Ptr<HTMLCollection> m_applets;
    GC::Ptr<HTMLCollection> m_anchors;
    GC::Ptr<HTMLCollection> m_images;
    GC::Ptr<HTMLCollection> m_embeds;
    GC::Ptr<HTMLCollection> m_links;
    GC::Ptr<HTMLCollection> m_forms;
    GC::Ptr<HTMLCollection> m_scripts;
    GC::Ptr<HTML::HTMLAllCollection> m_all;

    // https://drafts.csswg.org/css-font-loading/#font-source
    GC::Ref<CSS::FontFaceSet> m_fonts;

    // https://html.spec.whatwg.org/multipage/document-lifecycle.html#completely-loaded-time
    Optional<AK::UnixDateTime> m_completely_loaded_time;

    // https://html.spec.whatwg.org/multipage/dom.html#concept-document-navigation-id
    Optional<String> m_navigation_id;

    // https://html.spec.whatwg.org/multipage/origin.html#active-sandboxing-flag-set
    HTML::SandboxingFlagSet m_active_sandboxing_flag_set;

    // https://html.spec.whatwg.org/multipage/dom.html#concept-document-policy-container
    mutable GC::Ptr<HTML::PolicyContainer> m_policy_container;

    // https://html.spec.whatwg.org/multipage/interaction.html#visibility-state
    HTML::VisibilityState m_visibility_state { HTML::VisibilityState::Hidden };

    // https://html.spec.whatwg.org/multipage/dom.html#load-timing-info
    DocumentLoadTimingInfo m_load_timing_info;

    // https://html.spec.whatwg.org/multipage/dom.html#previous-document-unload-timing
    DocumentUnloadTimingInfo m_previous_document_unload_timing;

    // https://w3c.github.io/selection-api/#dfn-selection
    GC::Ptr<Selection::Selection> m_selection;

    // NOTE: This is a cache to make finding the first <base href> or <base target> element O(1).
    GC::Ptr<HTML::HTMLBaseElement> m_first_base_element_with_href_in_tree_order;
    GC::Ptr<HTML::HTMLBaseElement> m_first_base_element_with_target_in_tree_order;

    // https://html.spec.whatwg.org/multipage/images.html#list-of-available-images
    GC::Ptr<HTML::ListOfAvailableImages> m_list_of_available_images;

    GC::Ptr<CSS::VisualViewport> m_visual_viewport;

    // NOTE: Not in the spec per se, but Document must be able to access all IntersectionObservers whose root is in the document.
    IGNORE_GC OrderedHashTable<GC::Ref<IntersectionObserver::IntersectionObserver>> m_intersection_observers;

    // https://www.w3.org/TR/intersection-observer/#document-intersectionobservertaskqueued
    // Each document has an IntersectionObserverTaskQueued flag which is initialized to false.
    bool m_intersection_observer_task_queued { false };

    // https://html.spec.whatwg.org/multipage/urls-and-fetching.html#lazy-load-intersection-observer
    // Each Document has a lazy load intersection observer, initially set to null but can be set to an IntersectionObserver instance.
    GC::Ptr<IntersectionObserver::IntersectionObserver> m_lazy_load_intersection_observer;

    ResizeObserver::ResizeObserver::ResizeObserversList m_resize_observers;

    // https://html.spec.whatwg.org/multipage/semantics.html#will-declaratively-refresh
    // A Document object has an associated will declaratively refresh (a boolean). It is initially false.
    bool m_will_declaratively_refresh { false };

    RefPtr<Core::Timer> m_active_refresh_timer;

    TemporaryDocumentForFragmentParsing m_temporary_document_for_fragment_parsing { TemporaryDocumentForFragmentParsing::No };

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#latest-entry
    GC::Ptr<HTML::SessionHistoryEntry> m_latest_entry;

    HashMap<URL::URL, GC::Ptr<HTML::SharedResourceRequest>> m_shared_resource_requests;

    // https://www.w3.org/TR/web-animations-1/#timeline-associated-with-a-document
    HashTable<GC::Ref<Animations::AnimationTimeline>> m_associated_animation_timelines;

    // https://www.w3.org/TR/web-animations-1/#document-default-document-timeline
    GC::Ptr<Animations::DocumentTimeline> m_default_timeline;
    Optional<double> m_last_animation_frame_timestamp;

    // https://www.w3.org/TR/web-animations-1/#pending-animation-event-queue
    Vector<PendingAnimationEvent> m_pending_animation_event_queue;

    // https://drafts.csswg.org/css-transitions-2/#current-transition-generation
    size_t m_transition_generation { 0 };

    bool m_needs_to_call_page_did_load { false };

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#scripts-may-run-for-the-newly-created-document
    bool m_ready_to_run_scripts { false };

    Vector<HTML::FormAssociatedElement*> m_form_associated_elements_with_form_attribute;

    Vector<GC::Ref<DOM::Element>> m_potentially_named_elements;

    bool m_design_mode_enabled { false };

    bool m_needs_to_resolve_paint_only_properties { true };
    bool m_needs_accumulated_visual_contexts_update { false };
    bool m_needs_invalidation_of_elements_affected_by_has { false };

    mutable GC::Ptr<WebIDL::ObservableArray> m_adopted_style_sheets;

    // Document should not visit ShadowRoot list to avoid leaks.
    // It's responsibility of object that allocated ShadowRoot to keep it alive.
    ShadowRoot::DocumentShadowRootList m_shadow_roots;

    Optional<AK::UnixDateTime> m_last_modified;

    u64 m_dom_tree_version { 0 };
    u64 m_character_data_version { 0 };

    // https://drafts.csswg.org/css-position-4/#document-top-layer
    // Documents have a top layer, an ordered set containing elements from the document.
    // Elements in the top layer do not lay out normally based on their position in the document;
    // instead they generate boxes as if they were siblings of the root element.
    OrderedHashTable<GC::Ref<Element>> m_top_layer_elements;
    OrderedHashTable<GC::Ref<Element>> m_top_layer_pending_removals;

    Vector<GC::Ref<HTML::HTMLElement>> m_showing_auto_popover_list;
    Vector<GC::Ref<HTML::HTMLElement>> m_showing_hint_popover_list;

    GC::Ptr<HTML::HTMLElement> m_popover_pointerdown_target;

    Vector<GC::Ref<HTML::HTMLDialogElement>> m_open_dialogs_list;
    GC::Ptr<HTML::HTMLDialogElement> m_dialog_pointerdown_target;

    // https://dom.spec.whatwg.org/#document-allow-declarative-shadow-roots
    bool m_allow_declarative_shadow_roots { false };

    // https://w3c.github.io/selection-api/#dfn-has-scheduled-selectionchange-event
    bool m_has_scheduled_selectionchange_event { false };

    GC::Ptr<JS::ConsoleClient> m_console_client;

    RefPtr<Core::Timer> m_cursor_blink_timer;
    bool m_cursor_blink_state { false };

    // NOTE: This is GC::Weak, not GC::Ptr, on purpose. We don't want the document to keep some old detached navigable alive.
    GC::Weak<HTML::Navigable> m_cached_navigable;

    Core::SharedVersion m_cookie_version { Core::INVALID_SHARED_VERSION };
    Optional<Core::SharedVersionIndex> m_cookie_version_index;
    String m_cookie;

    Optional<HTML::PaintConfig> m_cached_display_list_paint_config;
    RefPtr<Painting::DisplayList> m_cached_display_list;

    mutable OwnPtr<Unicode::Segmenter> m_grapheme_segmenter;
    mutable OwnPtr<Unicode::Segmenter> m_line_segmenter;
    mutable OwnPtr<Unicode::Segmenter> m_word_segmenter;

    GC::Ref<EditingHostManager> m_editing_host_manager;

    bool m_inside_exec_command { false };

    // https://w3c.github.io/editing/docs/execCommand/#default-single-line-container-name
    FlyString m_default_single_line_container_name { HTML::TagNames::div };

    // https://w3c.github.io/editing/docs/execCommand/#css-styling-flag
    bool m_css_styling_flag { false };

    // https://w3c.github.io/editing/docs/execCommand/#state-override
    HashMap<FlyString, bool> m_command_state_override;

    // https://w3c.github.io/editing/docs/execCommand/#value-override
    HashMap<FlyString, Utf16String> m_command_value_override;

    // https://html.spec.whatwg.org/multipage/webstorage.html#session-storage-holder
    // A Document object has an associated session storage holder, which is null or a Storage object. It is initially null.
    GC::Ptr<HTML::Storage> m_session_storage_holder;

    // https://html.spec.whatwg.org/multipage/webstorage.html#local-storage-holder
    // A Document object has an associated local storage holder, which is null or a Storage object. It is initially null.
    GC::Ptr<HTML::Storage> m_local_storage_holder;

    // https://html.spec.whatwg.org/multipage/dom.html#render-blocking-element-set
    HashTable<GC::Ref<Element>> m_render_blocking_elements;

    // https://drafts.csswg.org/css-view-transitions-1/#document-active-view-transition
    GC::Ptr<ViewTransition::ViewTransition> m_active_view_transition;

    // https://drafts.csswg.org/css-view-transitions-1/#document-rendering-suppression-for-view-transitions
    bool m_rendering_suppression_for_view_transitions { false };

    // https://drafts.csswg.org/css-view-transitions-1/#document-dynamic-view-transition-style-sheet
    GC::Ptr<CSS::CSSStyleSheet> m_dynamic_view_transition_style_sheet;

    // https://drafts.csswg.org/css-view-transitions-1/#document-show-view-transition-tree
    bool m_show_view_transition_tree { false };

    // https://drafts.csswg.org/css-view-transitions-1/#document-update-callback-queue
    Vector<GC::Ptr<ViewTransition::ViewTransition>> m_update_callback_queue = {};

    GC::Ref<StyleInvalidator> m_style_invalidator;

    // https://www.w3.org/TR/css-properties-values-api-1/#dom-window-registeredpropertyset-slot
    HashMap<FlyString, CSS::CustomPropertyRegistration> m_registered_property_set;
    HashMap<FlyString, CSS::CustomPropertyRegistration> m_cached_registered_properties_from_css_property_rules;

    HashMap<FlyString, NonnullRefPtr<CSS::CounterStyle const>> m_registered_counter_styles;

    CSS::StyleScope m_style_scope;

    // https://drafts.csswg.org/css-values-5/#random-caching
    HashMap<CSS::RandomCachingKey, double> m_element_shared_css_random_base_value_cache;

    // https://fullscreen.spec.whatwg.org/#list-of-pending-fullscreen-events
    Vector<PendingFullscreenEvent> m_pending_fullscreen_events;
};

template<>
inline bool Node::fast_is<Document>() const { return is_document(); }

bool is_a_registrable_domain_suffix_of_or_is_equal_to(StringView host_suffix_string, URL::Host const& original_host);

}

namespace JS {

template<>
inline bool JS::Object::fast_is<Web::DOM::Document>() const { return is_dom_document(); }

}
