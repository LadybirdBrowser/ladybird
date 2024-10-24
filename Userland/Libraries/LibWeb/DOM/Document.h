/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023-2024, Shannon Booth <shannon@serenityos.org>
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
#include <LibCore/DateTime.h>
#include <LibCore/Forward.h>
#include <LibJS/Console.h>
#include <LibJS/Forward.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>
#include <LibUnicode/Forward.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/StyleSheetList.h>
#include <LibWeb/Cookie/Cookie.h>
#include <LibWeb/DOM/NonElementParentNode.h>
#include <LibWeb/DOM/ParentNode.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/CrossOrigin/OpenerPolicy.h>
#include <LibWeb/HTML/DocumentReadyState.h>
#include <LibWeb/HTML/HTMLScriptElement.h>
#include <LibWeb/HTML/History.h>
#include <LibWeb/HTML/LazyLoadingElement.h>
#include <LibWeb/HTML/NavigationType.h>
#include <LibWeb/HTML/SandboxingFlagSet.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/VisibilityState.h>
#include <LibWeb/InvalidateDisplayList.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/ObservableArray.h>

namespace Web::DOM {

enum class QuirksMode {
    No,
    Limited,
    Yes
};

// https://html.spec.whatwg.org/multipage/dom.html#document-load-timing-info
struct DocumentLoadTimingInfo {
    // https://html.spec.whatwg.org/multipage/dom.html#navigation-start-time
    double navigation_start_time { 0 };
    // https://html.spec.whatwg.org/multipage/dom.html#dom-interactive-time
    double dom_interactive_time { 0 };
    // https://html.spec.whatwg.org/multipage/dom.html#dom-content-loaded-event-start-time
    double dom_content_loaded_event_start_time { 0 };
    // https://html.spec.whatwg.org/multipage/dom.html#dom-content-loaded-event-end-time
    double dom_content_loaded_event_end_time { 0 };
    // https://html.spec.whatwg.org/multipage/dom.html#dom-complete-time
    double dom_complete_time { 0 };
    // https://html.spec.whatwg.org/multipage/dom.html#load-event-start-time
    double load_event_start_time { 0 };
    // https://html.spec.whatwg.org/multipage/dom.html#load-event-end-time
    double load_event_end_time { 0 };
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

enum class PolicyControlledFeature {
    Autoplay,
};

class Document
    : public ParentNode
    , public NonElementParentNode<Document>
    , public HTML::GlobalEventHandlers {
    WEB_PLATFORM_OBJECT(Document, ParentNode);
    JS_DECLARE_ALLOCATOR(Document);

public:
    enum class Type {
        XML,
        HTML
    };

    enum class TemporaryDocumentForFragmentParsing {
        No,
        Yes,
    };

    static WebIDL::ExceptionOr<JS::NonnullGCPtr<Document>> create_and_initialize(Type, String content_type, HTML::NavigationParams const&);

    [[nodiscard]] static JS::NonnullGCPtr<Document> create(JS::Realm&, URL::URL const& url = "about:blank"sv);
    [[nodiscard]] static JS::NonnullGCPtr<Document> create_for_fragment_parsing(JS::Realm&);
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<Document>> construct_impl(JS::Realm&);
    virtual ~Document() override;

    // AD-HOC: This number increments whenever a node is added or removed from the document, or an element attribute changes.
    //         It can be used as a crude invalidation mechanism for caches that depend on the DOM structure.
    u64 dom_tree_version() const { return m_dom_tree_version; }
    void bump_dom_tree_version() { ++m_dom_tree_version; }

    WebIDL::ExceptionOr<void> populate_with_html_head_and_body();

    JS::GCPtr<Selection::Selection> get_selection() const;

    WebIDL::ExceptionOr<String> cookie(Cookie::Source = Cookie::Source::NonHttp);
    WebIDL::ExceptionOr<void> set_cookie(StringView, Cookie::Source = Cookie::Source::NonHttp);
    bool is_cookie_averse() const;
    void enable_cookies_on_file_domains(Badge<Internals::Internals>) { m_enable_cookies_on_file_domains = true; }

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

    void set_url(const URL::URL& url) { m_url = url; }
    URL::URL url() const { return m_url; }
    URL::URL fallback_base_url() const;
    URL::URL base_url() const;

    void update_base_element(Badge<HTML::HTMLBaseElement>);
    JS::GCPtr<HTML::HTMLBaseElement const> first_base_element_with_href_in_tree_order() const;

    String url_string() const { return MUST(m_url.to_string()); }
    String document_uri() const { return url_string(); }

    URL::Origin origin() const;
    void set_origin(URL::Origin const& origin);

    HTML::OpenerPolicy const& opener_policy() const { return m_opener_policy; }
    void set_opener_policy(HTML::OpenerPolicy policy) { m_opener_policy = move(policy); }

    URL::URL parse_url(StringView) const;

    CSS::StyleComputer& style_computer() { return *m_style_computer; }
    const CSS::StyleComputer& style_computer() const { return *m_style_computer; }

    CSS::StyleSheetList& style_sheets();
    CSS::StyleSheetList const& style_sheets() const;

    void for_each_active_css_style_sheet(Function<void(CSS::CSSStyleSheet&, JS::GCPtr<DOM::ShadowRoot>)>&& callback) const;

    CSS::StyleSheetList* style_sheets_for_bindings() { return &style_sheets(); }

    Optional<String> get_style_sheet_source(CSS::StyleSheetIdentifier const&) const;

    virtual FlyString node_name() const override { return "#document"_fly_string; }

    void set_hovered_node(Node*);
    Node* hovered_node() { return m_hovered_node.ptr(); }
    Node const* hovered_node() const { return m_hovered_node.ptr(); }

    void set_inspected_node(Node*, Optional<CSS::Selector::PseudoElement::Type>);
    Node* inspected_node() { return m_inspected_node.ptr(); }
    Node const* inspected_node() const { return m_inspected_node.ptr(); }
    Layout::Node* inspected_layout_node();
    Layout::Node const* inspected_layout_node() const { return const_cast<Document*>(this)->inspected_layout_node(); }

    Element* document_element();
    Element const* document_element() const;

    HTML::HTMLHtmlElement* html_element();
    HTML::HTMLHeadElement* head();
    JS::GCPtr<HTML::HTMLTitleElement> title_element();

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

    JS::GCPtr<HTML::HTMLTitleElement const> title_element() const
    {
        return const_cast<Document*>(this)->title_element();
    }

    HTML::HTMLElement const* body() const
    {
        return const_cast<Document*>(this)->body();
    }

    WebIDL::ExceptionOr<void> set_body(HTML::HTMLElement* new_body);

    String title() const;
    WebIDL::ExceptionOr<void> set_title(String const&);

    HTML::BrowsingContext* browsing_context() { return m_browsing_context.ptr(); }
    HTML::BrowsingContext const* browsing_context() const { return m_browsing_context.ptr(); }

    void set_browsing_context(HTML::BrowsingContext*);

    Page& page();
    Page const& page() const;

    Color background_color() const;
    Vector<CSS::BackgroundLayerData> const* background_layers() const;

    Color normal_link_color() const;
    void set_normal_link_color(Color);

    Color active_link_color() const;
    void set_active_link_color(Color);

    Color visited_link_color() const;
    void set_visited_link_color(Color);

    void update_style();
    void update_layout();
    void update_paint_and_hit_testing_properties_if_needed();
    void update_animated_style_if_needed();

    void set_needs_layout();

    void invalidate_layout_tree();
    void invalidate_stacking_context_tree();

    virtual bool is_child_allowed(Node const&) const override;

    Layout::Viewport const* layout_node() const;
    Layout::Viewport* layout_node();

    Painting::ViewportPaintable const* paintable() const;
    Painting::ViewportPaintable* paintable();

    void schedule_style_update();
    void schedule_layout_update();

    JS::NonnullGCPtr<NodeList> get_elements_by_name(FlyString const&);

    JS::NonnullGCPtr<HTMLCollection> applets();
    JS::NonnullGCPtr<HTMLCollection> anchors();
    JS::NonnullGCPtr<HTMLCollection> images();
    JS::NonnullGCPtr<HTMLCollection> embeds();
    JS::NonnullGCPtr<HTMLCollection> plugins();
    JS::NonnullGCPtr<HTMLCollection> links();
    JS::NonnullGCPtr<HTMLCollection> forms();
    JS::NonnullGCPtr<HTMLCollection> scripts();
    JS::NonnullGCPtr<HTML::HTMLAllCollection> all();

    // https://drafts.csswg.org/css-font-loading/#font-source
    JS::NonnullGCPtr<CSS::FontFaceSet> fonts();

    void clear();
    void capture_events();
    void release_events();

    String const& source() const { return m_source; }
    void set_source(String source) { m_source = move(source); }

    HTML::EnvironmentSettingsObject& relevant_settings_object() const;

    WebIDL::ExceptionOr<JS::NonnullGCPtr<Element>> create_element(String const& local_name, Variant<String, ElementCreationOptions> const& options);
    WebIDL::ExceptionOr<JS::NonnullGCPtr<Element>> create_element_ns(Optional<FlyString> const& namespace_, String const& qualified_name, Variant<String, ElementCreationOptions> const& options);
    JS::NonnullGCPtr<DocumentFragment> create_document_fragment();
    JS::NonnullGCPtr<Text> create_text_node(String const& data);
    WebIDL::ExceptionOr<JS::NonnullGCPtr<CDATASection>> create_cdata_section(String const& data);
    JS::NonnullGCPtr<Comment> create_comment(String const& data);
    WebIDL::ExceptionOr<JS::NonnullGCPtr<ProcessingInstruction>> create_processing_instruction(String const& target, String const& data);

    WebIDL::ExceptionOr<JS::NonnullGCPtr<Attr>> create_attribute(String const& local_name);
    WebIDL::ExceptionOr<JS::NonnullGCPtr<Attr>> create_attribute_ns(Optional<FlyString> const& namespace_, String const& qualified_name);

    WebIDL::ExceptionOr<JS::NonnullGCPtr<Event>> create_event(StringView interface);
    JS::NonnullGCPtr<Range> create_range();

    void set_pending_parsing_blocking_script(HTML::HTMLScriptElement*);
    HTML::HTMLScriptElement* pending_parsing_blocking_script() { return m_pending_parsing_blocking_script.ptr(); }
    JS::NonnullGCPtr<HTML::HTMLScriptElement> take_pending_parsing_blocking_script(Badge<HTML::HTMLParser>);

    void add_script_to_execute_when_parsing_has_finished(Badge<HTML::HTMLScriptElement>, HTML::HTMLScriptElement&);
    Vector<JS::Handle<HTML::HTMLScriptElement>> take_scripts_to_execute_when_parsing_has_finished(Badge<HTML::HTMLParser>);
    Vector<JS::NonnullGCPtr<HTML::HTMLScriptElement>>& scripts_to_execute_when_parsing_has_finished() { return m_scripts_to_execute_when_parsing_has_finished; }

    void add_script_to_execute_as_soon_as_possible(Badge<HTML::HTMLScriptElement>, HTML::HTMLScriptElement&);
    Vector<JS::Handle<HTML::HTMLScriptElement>> take_scripts_to_execute_as_soon_as_possible(Badge<HTML::HTMLParser>);
    Vector<JS::NonnullGCPtr<HTML::HTMLScriptElement>>& scripts_to_execute_as_soon_as_possible() { return m_scripts_to_execute_as_soon_as_possible; }

    void add_script_to_execute_in_order_as_soon_as_possible(Badge<HTML::HTMLScriptElement>, HTML::HTMLScriptElement&);
    Vector<JS::Handle<HTML::HTMLScriptElement>> take_scripts_to_execute_in_order_as_soon_as_possible(Badge<HTML::HTMLParser>);
    Vector<JS::NonnullGCPtr<HTML::HTMLScriptElement>>& scripts_to_execute_in_order_as_soon_as_possible() { return m_scripts_to_execute_in_order_as_soon_as_possible; }

    QuirksMode mode() const { return m_quirks_mode; }
    bool in_quirks_mode() const { return m_quirks_mode == QuirksMode::Yes; }
    void set_quirks_mode(QuirksMode mode) { m_quirks_mode = mode; }

    Type document_type() const { return m_type; }
    void set_document_type(Type type) { m_type = type; }

    // https://dom.spec.whatwg.org/#html-document
    bool is_html_document() const { return m_type == Type::HTML; }

    // https://dom.spec.whatwg.org/#xml-document
    bool is_xml_document() const { return m_type == Type::XML; }

    WebIDL::ExceptionOr<JS::NonnullGCPtr<Node>> import_node(JS::NonnullGCPtr<Node> node, bool deep);
    void adopt_node(Node&);
    WebIDL::ExceptionOr<JS::NonnullGCPtr<Node>> adopt_node_binding(JS::NonnullGCPtr<Node>);

    DocumentType const* doctype() const;
    String const& compat_mode() const;

    void set_editable(bool editable) { m_editable = editable; }
    virtual bool is_editable() const final;

    Element* focused_element() { return m_focused_element.ptr(); }
    Element const* focused_element() const { return m_focused_element.ptr(); }

    void set_focused_element(Element*);

    Element const* active_element() const { return m_active_element.ptr(); }

    void set_active_element(Element*);

    Element const* target_element() const { return m_target_element.ptr(); }
    void set_target_element(Element*);

    void try_to_scroll_to_the_fragment();
    void scroll_to_the_fragment();
    void scroll_to_the_beginning_of_the_document();

    bool created_for_appropriate_template_contents() const { return m_created_for_appropriate_template_contents; }

    JS::NonnullGCPtr<Document> appropriate_template_contents_owner_document();

    StringView ready_state() const;
    HTML::DocumentReadyState readiness() const { return m_readiness; }
    void update_readiness(HTML::DocumentReadyState);

    String last_modified() const;

    [[nodiscard]] JS::GCPtr<HTML::Window> window() const { return m_window; }

    void set_window(HTML::Window&);

    WebIDL::ExceptionOr<void> write(Vector<String> const& strings);
    WebIDL::ExceptionOr<void> writeln(Vector<String> const& strings);

    WebIDL::ExceptionOr<Document*> open(Optional<String> const& = {}, Optional<String> const& = {});
    WebIDL::ExceptionOr<JS::GCPtr<HTML::WindowProxy>> open(StringView url, StringView name, StringView features);
    WebIDL::ExceptionOr<void> close();

    JS::GCPtr<HTML::WindowProxy const> default_view() const;
    JS::GCPtr<HTML::WindowProxy> default_view();

    String const& content_type() const { return m_content_type; }
    void set_content_type(String content_type) { m_content_type = move(content_type); }

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

    JS::GCPtr<HTML::HTMLScriptElement> current_script() const { return m_current_script.ptr(); }
    void set_current_script(Badge<HTML::HTMLScriptElement>, JS::GCPtr<HTML::HTMLScriptElement> script) { m_current_script = move(script); }

    u32 ignore_destructive_writes_counter() const { return m_ignore_destructive_writes_counter; }
    void increment_ignore_destructive_writes_counter() { m_ignore_destructive_writes_counter++; }
    void decrement_ignore_destructive_writes_counter() { m_ignore_destructive_writes_counter--; }

    virtual EventTarget* get_parent(Event const&) override;

    String dump_dom_tree_as_json() const;

    bool has_a_style_sheet_that_is_blocking_scripts() const;

    bool is_fully_active() const;
    bool is_active() const;

    [[nodiscard]] bool allow_declarative_shadow_roots() const;
    void set_allow_declarative_shadow_roots(bool);

    JS::NonnullGCPtr<HTML::History> history();
    JS::NonnullGCPtr<HTML::History> history() const;

    [[nodiscard]] JS::GCPtr<HTML::Location> location();

    bool anything_is_delaying_the_load_event() const;
    void increment_number_of_things_delaying_the_load_event(Badge<DocumentLoadEventDelayer>);
    void decrement_number_of_things_delaying_the_load_event(Badge<DocumentLoadEventDelayer>);

    bool page_showing() const { return m_page_showing; }
    void set_page_showing(bool value) { m_page_showing = value; }

    bool hidden() const;
    StringView visibility_state() const;

    // https://html.spec.whatwg.org/multipage/interaction.html#update-the-visibility-state
    void update_the_visibility_state(HTML::VisibilityState);

    // NOTE: This does not fire any events, unlike update_the_visibility_state().
    void set_visibility_state(Badge<HTML::BrowsingContext>, HTML::VisibilityState);

    void run_the_resize_steps();
    void run_the_scroll_steps();

    void evaluate_media_queries_and_report_changes();
    void add_media_query_list(JS::NonnullGCPtr<CSS::MediaQueryList>);

    JS::NonnullGCPtr<CSS::VisualViewport> visual_viewport();
    [[nodiscard]] CSSPixelRect viewport_rect() const;

    class ViewportClient {
    public:
        virtual ~ViewportClient() = default;
        virtual void did_set_viewport_rect(CSSPixelRect const&) = 0;
    };
    void register_viewport_client(ViewportClient&);
    void unregister_viewport_client(ViewportClient&);
    void inform_all_viewport_clients_about_the_current_viewport_rect();

    bool has_focus() const;

    void set_parser(Badge<HTML::HTMLParser>, HTML::HTMLParser&);
    void detach_parser(Badge<HTML::HTMLParser>);

    [[nodiscard]] bool is_temporary_document_for_fragment_parsing() const { return m_temporary_document_for_fragment_parsing == TemporaryDocumentForFragmentParsing::Yes; }

    static bool is_valid_name(String const&);

    struct PrefixAndTagName {
        FlyString prefix;
        FlyString tag_name;
    };
    static WebIDL::ExceptionOr<PrefixAndTagName> validate_qualified_name(JS::Realm&, FlyString const& qualified_name);

    JS::NonnullGCPtr<NodeIterator> create_node_iterator(Node& root, unsigned what_to_show, JS::GCPtr<NodeFilter>);
    JS::NonnullGCPtr<TreeWalker> create_tree_walker(Node& root, unsigned what_to_show, JS::GCPtr<NodeFilter>);

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

    void set_needs_to_refresh_scroll_state(bool b);

    bool has_active_favicon() const { return m_active_favicon; }
    void check_favicon_after_loading_link_resource();

    JS::GCPtr<HTML::CustomElementDefinition> lookup_custom_element_definition(Optional<FlyString> const& namespace_, FlyString const& local_name, Optional<String> const& is) const;

    void increment_throw_on_dynamic_markup_insertion_counter(Badge<HTML::HTMLParser>);
    void decrement_throw_on_dynamic_markup_insertion_counter(Badge<HTML::HTMLParser>);

    // https://html.spec.whatwg.org/multipage/dom.html#is-initial-about:blank
    bool is_initial_about_blank() const { return m_is_initial_about_blank; }
    void set_is_initial_about_blank(bool b) { m_is_initial_about_blank = b; }

    // https://html.spec.whatwg.org/multipage/dom.html#concept-document-about-base-url
    Optional<URL::URL> about_base_url() const { return m_about_base_url; }
    void set_about_base_url(Optional<URL::URL> url) { m_about_base_url = url; }

    String domain() const;
    void set_domain(String const&);

    auto& pending_scroll_event_targets() { return m_pending_scroll_event_targets; }
    auto& pending_scrollend_event_targets() { return m_pending_scrollend_event_targets; }

    // https://html.spec.whatwg.org/#completely-loaded
    bool is_completely_loaded() const;

    // https://html.spec.whatwg.org/multipage/dom.html#concept-document-navigation-id
    Optional<String> navigation_id() const;
    void set_navigation_id(Optional<String>);

    // https://html.spec.whatwg.org/multipage/origin.html#active-sandboxing-flag-set
    HTML::SandboxingFlagSet active_sandboxing_flag_set() const;

    // https://html.spec.whatwg.org/multipage/dom.html#concept-document-policy-container
    HTML::PolicyContainer policy_container() const;
    void set_policy_container(HTML::PolicyContainer);

    Vector<JS::Handle<HTML::Navigable>> descendant_navigables();
    Vector<JS::Handle<HTML::Navigable>> const descendant_navigables() const;
    Vector<JS::Handle<HTML::Navigable>> inclusive_descendant_navigables();
    Vector<JS::Handle<HTML::Navigable>> ancestor_navigables();
    Vector<JS::Handle<HTML::Navigable>> const ancestor_navigables() const;
    Vector<JS::Handle<HTML::Navigable>> inclusive_ancestor_navigables();
    Vector<JS::Handle<HTML::Navigable>> document_tree_child_navigables();

    // https://html.spec.whatwg.org/multipage/document-lifecycle.html#destroy-a-document
    void destroy();
    // https://html.spec.whatwg.org/multipage/document-lifecycle.html#destroy-a-document-and-its-descendants
    void destroy_a_document_and_its_descendants(JS::GCPtr<JS::HeapFunction<void()>> after_all_destruction = {});

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#abort-a-document
    void abort();
    // https://html.spec.whatwg.org/multipage/document-lifecycle.html#abort-a-document-and-its-descendants
    void abort_a_document_and_its_descendants();

    // https://html.spec.whatwg.org/multipage/document-lifecycle.html#unload-a-document
    void unload(JS::GCPtr<Document> new_document = nullptr);
    // https://html.spec.whatwg.org/multipage/document-lifecycle.html#unload-a-document-and-its-descendants
    void unload_a_document_and_its_descendants(JS::GCPtr<Document> new_document, JS::GCPtr<JS::HeapFunction<void()>> after_all_unloads = {});

    // https://html.spec.whatwg.org/multipage/dom.html#active-parser
    JS::GCPtr<HTML::HTMLParser> active_parser();

    // https://html.spec.whatwg.org/multipage/dom.html#load-timing-info
    DocumentLoadTimingInfo& load_timing_info() { return m_load_timing_info; }
    DocumentLoadTimingInfo const& load_timing_info() const { return m_load_timing_info; }
    void set_load_timing_info(DocumentLoadTimingInfo const& load_timing_info) { m_load_timing_info = load_timing_info; }

    // https://html.spec.whatwg.org/multipage/dom.html#previous-document-unload-timing
    DocumentUnloadTimingInfo& previous_document_unload_timing() { return m_previous_document_unload_timing; }
    DocumentUnloadTimingInfo const& previous_document_unload_timing() const { return m_previous_document_unload_timing; }
    void set_previous_document_unload_timing(DocumentUnloadTimingInfo const& previous_document_unload_timing) { m_previous_document_unload_timing = previous_document_unload_timing; }

    // https://w3c.github.io/editing/docs/execCommand/
    bool exec_command(String const& command, bool show_ui, String const& value);
    bool query_command_enabled(String const& command);
    bool query_command_indeterm(String const& command);
    bool query_command_state(String const& command);
    bool query_command_supported(String const& command);
    String query_command_value(String const& command);

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

    void shared_declarative_refresh_steps(StringView input, JS::GCPtr<HTML::HTMLMetaElement const> meta_element = nullptr);

    struct TopOfTheDocument { };
    using IndicatedPart = Variant<Element*, TopOfTheDocument>;
    IndicatedPart determine_the_indicated_part() const;

    u32 unload_counter() const { return m_unload_counter; }

    HTML::SourceSnapshotParams snapshot_source_snapshot_params() const;

    void update_for_history_step_application(JS::NonnullGCPtr<HTML::SessionHistoryEntry>, bool do_not_reactivate, size_t script_history_length, size_t script_history_index, Optional<Bindings::NavigationType> navigation_type, Optional<Vector<JS::NonnullGCPtr<HTML::SessionHistoryEntry>>> entries_for_navigation_api = {}, Optional<JS::NonnullGCPtr<HTML::SessionHistoryEntry>> previous_entry_for_activation = {}, bool update_navigation_api = true);

    HashMap<URL::URL, JS::GCPtr<HTML::SharedResourceRequest>>& shared_resource_requests();

    void restore_the_history_object_state(JS::NonnullGCPtr<HTML::SessionHistoryEntry> entry);

    JS::NonnullGCPtr<Animations::DocumentTimeline> timeline();
    auto const& last_animation_frame_timestamp() const { return m_last_animation_frame_timestamp; }

    void associate_with_timeline(JS::NonnullGCPtr<Animations::AnimationTimeline>);
    void disassociate_with_timeline(JS::NonnullGCPtr<Animations::AnimationTimeline>);

    struct PendingAnimationEvent {
        JS::NonnullGCPtr<DOM::Event> event;
        JS::NonnullGCPtr<Animations::Animation> animation;
        JS::NonnullGCPtr<DOM::EventTarget> target;
        Optional<double> scheduled_event_time;
    };
    void append_pending_animation_event(PendingAnimationEvent const&);
    void update_animations_and_send_events(Optional<double> const& timestamp);
    void remove_replaced_animations();

    Vector<JS::NonnullGCPtr<Animations::Animation>> get_animations();

    bool ready_to_run_scripts() const { return m_ready_to_run_scripts; }
    void set_ready_to_run_scripts() { m_ready_to_run_scripts = true; }

    JS::GCPtr<HTML::SessionHistoryEntry> latest_entry() const { return m_latest_entry; }
    void set_latest_entry(JS::GCPtr<HTML::SessionHistoryEntry> e) { m_latest_entry = e; }

    void element_id_changed(Badge<DOM::Element>, JS::NonnullGCPtr<DOM::Element> element);
    void element_with_id_was_added(Badge<DOM::Element>, JS::NonnullGCPtr<DOM::Element> element);
    void element_with_id_was_removed(Badge<DOM::Element>, JS::NonnullGCPtr<DOM::Element> element);
    void element_name_changed(Badge<DOM::Element>, JS::NonnullGCPtr<DOM::Element> element);
    void element_with_name_was_added(Badge<DOM::Element>, JS::NonnullGCPtr<DOM::Element> element);
    void element_with_name_was_removed(Badge<DOM::Element>, JS::NonnullGCPtr<DOM::Element> element);

    void add_form_associated_element_with_form_attribute(HTML::FormAssociatedElement&);
    void remove_form_associated_element_with_form_attribute(HTML::FormAssociatedElement&);

    bool design_mode_enabled_state() const { return m_design_mode_enabled; }
    void set_design_mode_enabled_state(bool);
    String design_mode() const;
    WebIDL::ExceptionOr<void> set_design_mode(String const&);

    Element const* element_from_point(double x, double y);
    JS::MarkedVector<JS::NonnullGCPtr<Element>> elements_from_point(double x, double y);
    JS::GCPtr<Element const> scrolling_element() const;

    void set_needs_to_resolve_paint_only_properties() { m_needs_to_resolve_paint_only_properties = true; }
    void set_needs_animated_style_update() { m_needs_animated_style_update = true; }

    virtual JS::Value named_item_value(FlyString const& name) const override;
    virtual Vector<FlyString> supported_property_names() const override;
    Vector<JS::NonnullGCPtr<DOM::Element>> const& potentially_named_elements() const { return m_potentially_named_elements; }

    void gather_active_observations_at_depth(size_t depth);
    [[nodiscard]] size_t broadcast_active_resize_observations();
    [[nodiscard]] bool has_active_resize_observations();
    [[nodiscard]] bool has_skipped_resize_observations();

    JS::NonnullGCPtr<WebIDL::ObservableArray> adopted_style_sheets() const;
    WebIDL::ExceptionOr<void> set_adopted_style_sheets(JS::Value);

    void register_shadow_root(Badge<DOM::ShadowRoot>, DOM::ShadowRoot&);
    void unregister_shadow_root(Badge<DOM::ShadowRoot>, DOM::ShadowRoot&);
    void for_each_shadow_root(Function<void(DOM::ShadowRoot&)>&& callback);
    void for_each_shadow_root(Function<void(DOM::ShadowRoot&)>&& callback) const;

    void add_an_element_to_the_top_layer(JS::NonnullGCPtr<Element>);
    void request_an_element_to_be_remove_from_the_top_layer(JS::NonnullGCPtr<Element>);
    void remove_an_element_from_the_top_layer_immediately(JS::NonnullGCPtr<Element>);
    void process_top_layer_removals();

    OrderedHashTable<JS::NonnullGCPtr<Element>> const& top_layer_elements() const { return m_top_layer_elements; }

    size_t transition_generation() const { return m_transition_generation; }

    // Does document represent an embedded svg img
    [[nodiscard]] bool is_decoded_svg() const;

    Vector<JS::Handle<DOM::Range>> find_matching_text(String const&, CaseSensitivity);

    void parse_html_from_a_string(StringView);
    static JS::NonnullGCPtr<Document> parse_html_unsafe(JS::VM&, StringView);

    void set_console_client(JS::GCPtr<JS::ConsoleClient> console_client) { m_console_client = console_client; }
    JS::GCPtr<JS::ConsoleClient> console_client() const { return m_console_client; }

    JS::GCPtr<DOM::Position> cursor_position() const { return m_cursor_position; }
    void set_cursor_position(JS::NonnullGCPtr<DOM::Position>);
    bool increment_cursor_position_offset();
    bool decrement_cursor_position_offset();
    bool increment_cursor_position_to_next_word();
    bool decrement_cursor_position_to_previous_word();

    bool cursor_blink_state() const { return m_cursor_blink_state; }

    void user_did_edit_document_text(Badge<EditEventHandler>);
    // Cached pointer to the last known node navigable.
    // If this document is currently the "active document" of the cached navigable, the cache is still valid.
    JS::GCPtr<HTML::Navigable> cached_navigable();
    void set_cached_navigable(JS::GCPtr<HTML::Navigable>);

    [[nodiscard]] bool needs_repaint() const { return m_needs_repaint; }
    void set_needs_display(InvalidateDisplayList = InvalidateDisplayList::Yes);
    void set_needs_display(CSSPixelRect const&, InvalidateDisplayList = InvalidateDisplayList::Yes);

    struct PaintConfig {
        bool paint_overlay { false };
        bool should_show_line_box_borders { false };
        bool has_focus { false };
        Optional<Gfx::IntRect> canvas_fill_rect {};

        bool operator==(PaintConfig const& other) const = default;
    };
    RefPtr<Painting::DisplayList> record_display_list(PaintConfig);

    void invalidate_display_list();

    Unicode::Segmenter& grapheme_segmenter() const;
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

protected:
    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    Document(JS::Realm&, URL::URL const&, TemporaryDocumentForFragmentParsing = TemporaryDocumentForFragmentParsing::No);

private:
    // ^HTML::GlobalEventHandlers
    virtual JS::GCPtr<EventTarget> global_event_handlers_to_event_target(FlyString const&) final { return *this; }

    void tear_down_layout_tree();

    void update_active_element();

    void run_unloading_cleanup_steps();

    void evaluate_media_rules();

    WebIDL::ExceptionOr<void> run_the_document_write_steps(StringView);

    void queue_intersection_observer_task();
    void queue_an_intersection_observer_entry(IntersectionObserver::IntersectionObserver&, HighResolutionTime::DOMHighResTimeStamp time, JS::NonnullGCPtr<Geometry::DOMRectReadOnly> root_bounds, JS::NonnullGCPtr<Geometry::DOMRectReadOnly> bounding_client_rect, JS::NonnullGCPtr<Geometry::DOMRectReadOnly> intersection_rect, bool is_intersecting, double intersection_ratio, JS::NonnullGCPtr<Element> target);

    Element* find_a_potential_indicated_element(FlyString const& fragment) const;

    void dispatch_events_for_animation_if_necessary(JS::NonnullGCPtr<Animations::Animation>);

    void reset_cursor_blink_cycle();

    JS::NonnullGCPtr<Page> m_page;
    OwnPtr<CSS::StyleComputer> m_style_computer;
    JS::GCPtr<CSS::StyleSheetList> m_style_sheets;
    JS::GCPtr<Node> m_hovered_node;
    JS::GCPtr<Node> m_inspected_node;
    Optional<CSS::Selector::PseudoElement::Type> m_inspected_pseudo_element;
    JS::GCPtr<Node> m_active_favicon;
    WeakPtr<HTML::BrowsingContext> m_browsing_context;
    URL::URL m_url;

    JS::GCPtr<HTML::Window> m_window;

    JS::GCPtr<Layout::Viewport> m_layout_root;

    Optional<Color> m_normal_link_color;
    Optional<Color> m_active_link_color;
    Optional<Color> m_visited_link_color;

    JS::GCPtr<HTML::HTMLParser> m_parser;
    bool m_active_parser_was_aborted { false };

    String m_source;

    JS::GCPtr<HTML::HTMLScriptElement> m_pending_parsing_blocking_script;

    Vector<JS::NonnullGCPtr<HTML::HTMLScriptElement>> m_scripts_to_execute_when_parsing_has_finished;

    // https://html.spec.whatwg.org/multipage/scripting.html#list-of-scripts-that-will-execute-in-order-as-soon-as-possible
    Vector<JS::NonnullGCPtr<HTML::HTMLScriptElement>> m_scripts_to_execute_in_order_as_soon_as_possible;

    // https://html.spec.whatwg.org/multipage/scripting.html#set-of-scripts-that-will-execute-as-soon-as-possible
    Vector<JS::NonnullGCPtr<HTML::HTMLScriptElement>> m_scripts_to_execute_as_soon_as_possible;

    QuirksMode m_quirks_mode { QuirksMode::No };

    // https://dom.spec.whatwg.org/#concept-document-type
    Type m_type { Type::XML };

    bool m_editable { false };

    JS::GCPtr<Element> m_focused_element;
    JS::GCPtr<Element> m_active_element;
    JS::GCPtr<Element> m_target_element;

    bool m_created_for_appropriate_template_contents { false };
    JS::GCPtr<Document> m_associated_inert_template_document;
    JS::GCPtr<Document> m_appropriate_template_contents_owner_document;

    HTML::DocumentReadyState m_readiness { HTML::DocumentReadyState::Loading };
    String m_content_type { "application/xml"_string };
    Optional<String> m_encoding;

    bool m_ready_for_post_load_tasks { false };

    JS::GCPtr<DOMImplementation> m_implementation;
    JS::GCPtr<HTML::HTMLScriptElement> m_current_script;

    bool m_should_invalidate_styles_on_attribute_changes { true };

    u32 m_ignore_destructive_writes_counter { 0 };

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#unload-counter
    u32 m_unload_counter { 0 };

    // https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#throw-on-dynamic-markup-insertion-counter
    u32 m_throw_on_dynamic_markup_insertion_counter { 0 };

    // https://html.spec.whatwg.org/multipage/semantics.html#script-blocking-style-sheet-counter
    u32 m_script_blocking_style_sheet_counter { 0 };

    JS::GCPtr<HTML::History> m_history;

    size_t m_number_of_things_delaying_the_load_event { 0 };

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#concept-document-salvageable
    bool m_salvageable { true };

    // https://html.spec.whatwg.org/#page-showing
    bool m_page_showing { false };

    // Used by run_the_resize_steps().
    Optional<Gfx::IntSize> m_last_viewport_size;

    HashTable<ViewportClient*> m_viewport_clients;

    // https://w3c.github.io/csswg-drafts/cssom-view-1/#document-pending-scroll-event-targets
    Vector<JS::NonnullGCPtr<EventTarget>> m_pending_scroll_event_targets;

    // https://w3c.github.io/csswg-drafts/cssom-view-1/#document-pending-scrollend-event-targets
    Vector<JS::NonnullGCPtr<EventTarget>> m_pending_scrollend_event_targets;

    // Used by evaluate_media_queries_and_report_changes().
    Vector<WeakPtr<CSS::MediaQueryList>> m_media_query_lists;

    bool m_needs_layout { false };

    bool m_needs_full_style_update { false };

    bool m_needs_animated_style_update { false };

    HashTable<JS::GCPtr<NodeIterator>> m_node_iterators;

    HashTable<JS::NonnullGCPtr<DocumentObserver>> m_document_observers;

    // https://html.spec.whatwg.org/multipage/dom.html#is-initial-about:blank
    bool m_is_initial_about_blank { false };

    // https://html.spec.whatwg.org/multipage/dom.html#concept-document-about-base-url
    Optional<URL::URL> m_about_base_url;

    // https://html.spec.whatwg.org/multipage/dom.html#concept-document-coop
    HTML::OpenerPolicy m_opener_policy;

    // https://html.spec.whatwg.org/multipage/dom.html#the-document's-referrer
    String m_referrer;

    // https://dom.spec.whatwg.org/#concept-document-origin
    URL::Origin m_origin;

    JS::GCPtr<HTMLCollection> m_applets;
    JS::GCPtr<HTMLCollection> m_anchors;
    JS::GCPtr<HTMLCollection> m_images;
    JS::GCPtr<HTMLCollection> m_embeds;
    JS::GCPtr<HTMLCollection> m_links;
    JS::GCPtr<HTMLCollection> m_forms;
    JS::GCPtr<HTMLCollection> m_scripts;
    JS::GCPtr<HTML::HTMLAllCollection> m_all;

    // https://drafts.csswg.org/css-font-loading/#font-source
    JS::GCPtr<CSS::FontFaceSet> m_fonts;

    // https://html.spec.whatwg.org/#completely-loaded-time
    Optional<AK::UnixDateTime> m_completely_loaded_time;

    // https://html.spec.whatwg.org/multipage/dom.html#concept-document-navigation-id
    Optional<String> m_navigation_id;

    // https://html.spec.whatwg.org/multipage/origin.html#active-sandboxing-flag-set
    HTML::SandboxingFlagSet m_active_sandboxing_flag_set;

    // https://html.spec.whatwg.org/multipage/dom.html#concept-document-policy-container
    HTML::PolicyContainer m_policy_container;

    // https://html.spec.whatwg.org/multipage/interaction.html#visibility-state
    HTML::VisibilityState m_visibility_state { HTML::VisibilityState::Hidden };

    // https://html.spec.whatwg.org/multipage/dom.html#load-timing-info
    DocumentLoadTimingInfo m_load_timing_info;

    // https://html.spec.whatwg.org/multipage/dom.html#previous-document-unload-timing
    DocumentUnloadTimingInfo m_previous_document_unload_timing;

    // https://w3c.github.io/selection-api/#dfn-selection
    JS::GCPtr<Selection::Selection> m_selection;

    // NOTE: This is a cache to make finding the first <base href> element O(1).
    JS::GCPtr<HTML::HTMLBaseElement const> m_first_base_element_with_href_in_tree_order;

    // https://html.spec.whatwg.org/multipage/images.html#list-of-available-images
    JS::GCPtr<HTML::ListOfAvailableImages> m_list_of_available_images;

    JS::GCPtr<CSS::VisualViewport> m_visual_viewport;

    // NOTE: Not in the spec per say, but Document must be able to access all IntersectionObservers whose root is in the document.
    OrderedHashTable<JS::NonnullGCPtr<IntersectionObserver::IntersectionObserver>> m_intersection_observers;

    // https://www.w3.org/TR/intersection-observer/#document-intersectionobservertaskqueued
    // Each document has an IntersectionObserverTaskQueued flag which is initialized to false.
    bool m_intersection_observer_task_queued { false };

    // https://html.spec.whatwg.org/multipage/urls-and-fetching.html#lazy-load-intersection-observer
    // Each Document has a lazy load intersection observer, initially set to null but can be set to an IntersectionObserver instance.
    JS::GCPtr<IntersectionObserver::IntersectionObserver> m_lazy_load_intersection_observer;

    Vector<JS::NonnullGCPtr<ResizeObserver::ResizeObserver>> m_resize_observers;

    // https://html.spec.whatwg.org/multipage/semantics.html#will-declaratively-refresh
    // A Document object has an associated will declaratively refresh (a boolean). It is initially false.
    bool m_will_declaratively_refresh { false };

    RefPtr<Core::Timer> m_active_refresh_timer;

    TemporaryDocumentForFragmentParsing m_temporary_document_for_fragment_parsing { TemporaryDocumentForFragmentParsing::No };

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#latest-entry
    JS::GCPtr<HTML::SessionHistoryEntry> m_latest_entry;

    HashMap<URL::URL, JS::GCPtr<HTML::SharedResourceRequest>> m_shared_resource_requests;

    // https://www.w3.org/TR/web-animations-1/#timeline-associated-with-a-document
    HashTable<JS::NonnullGCPtr<Animations::AnimationTimeline>> m_associated_animation_timelines;

    // https://www.w3.org/TR/web-animations-1/#document-default-document-timeline
    JS::GCPtr<Animations::DocumentTimeline> m_default_timeline;
    Optional<double> m_last_animation_frame_timestamp;

    // https://www.w3.org/TR/web-animations-1/#pending-animation-event-queue
    Vector<PendingAnimationEvent> m_pending_animation_event_queue;
    RefPtr<Core::Timer> m_animation_driver_timer;

    // https://drafts.csswg.org/css-transitions-2/#current-transition-generation
    size_t m_transition_generation { 0 };

    bool m_needs_to_call_page_did_load { false };

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#scripts-may-run-for-the-newly-created-document
    bool m_ready_to_run_scripts { false };

    Vector<HTML::FormAssociatedElement*> m_form_associated_elements_with_form_attribute;

    Vector<JS::NonnullGCPtr<DOM::Element>> m_potentially_named_elements;

    bool m_design_mode_enabled { false };

    bool m_needs_to_resolve_paint_only_properties { true };

    mutable JS::GCPtr<WebIDL::ObservableArray> m_adopted_style_sheets;

    Vector<JS::NonnullGCPtr<DOM::ShadowRoot>> m_shadow_roots;

    Optional<Core::DateTime> m_last_modified;

    u64 m_dom_tree_version { 0 };

    // https://drafts.csswg.org/css-position-4/#document-top-layer
    // Documents have a top layer, an ordered set containing elements from the document.
    // Elements in the top layer do not lay out normally based on their position in the document;
    // instead they generate boxes as if they were siblings of the root element.
    OrderedHashTable<JS::NonnullGCPtr<Element>> m_top_layer_elements;
    OrderedHashTable<JS::NonnullGCPtr<Element>> m_top_layer_pending_removals;

    // https://dom.spec.whatwg.org/#document-allow-declarative-shadow-roots
    bool m_allow_declarative_shadow_roots { false };

    // https://w3c.github.io/selection-api/#dfn-has-scheduled-selectionchange-event
    bool m_has_scheduled_selectionchange_event { false };

    JS::GCPtr<JS::ConsoleClient> m_console_client;

    JS::GCPtr<DOM::Position> m_cursor_position;
    RefPtr<Core::Timer> m_cursor_blink_timer;
    bool m_cursor_blink_state { false };

    // NOTE: This is WeakPtr, not GCPtr, on purpose. We don't want the document to keep some old detached navigable alive.
    WeakPtr<HTML::Navigable> m_cached_navigable;

    bool m_needs_repaint { false };

    bool m_enable_cookies_on_file_domains { false };

    Optional<PaintConfig> m_cached_display_list_paint_config;
    RefPtr<Painting::DisplayList> m_cached_display_list;

    mutable OwnPtr<Unicode::Segmenter> m_grapheme_segmenter;
    mutable OwnPtr<Unicode::Segmenter> m_word_segmenter;
};

template<>
inline bool Node::fast_is<Document>() const { return is_document(); }

}
