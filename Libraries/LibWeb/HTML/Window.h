/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/RefPtr.h>
#include <AK/TypeCasts.h>
#include <LibGC/Heap.h>
#include <LibURL/URL.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WindowGlobalMixin.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/BarProp.h>
#include <LibWeb/HTML/CrossOrigin/CrossOriginPropertyDescriptorMap.h>
#include <LibWeb/HTML/GlobalEventHandlers.h>
#include <LibWeb/HTML/MimeType.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/Plugin.h>
#include <LibWeb/HTML/ScrollOptions.h>
#include <LibWeb/HTML/StructuredSerializeOptions.h>
#include <LibWeb/HTML/UniversalGlobalScope.h>
#include <LibWeb/HTML/WindowEventHandlers.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/RequestIdleCallback/IdleRequest.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::HTML {

class IdleCallback;

// https://w3c.github.io/csswg-drafts/cssom-view/#dictdef-scrolltooptions
struct ScrollToOptions : public ScrollOptions {
    Optional<double> left;
    Optional<double> top;
};

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#windowpostmessageoptions
struct WindowPostMessageOptions : public StructuredSerializeOptions {
    String target_origin { "/"_string };
};

// https://html.spec.whatwg.org/multipage/webappapis.html#specifier-resolution-record
// A specifier resolution record is a struct. It has the following items:
struct SpecifierResolution {
    // A serialized base URL
    //    A string-or-null that represents the base URL of the specifier, when one exists.
    Optional<String> serialized_base_url;

    // A specifier
    //     A string representing the specifier.
    String specifier;

    // A specifier as a URL
    //     A URL-or-null that represents the URL in case of a URL-like module specifier.
    //
    // Spec-Note: Implementations can replace specifier as a URL with a boolean that indicates
    //            that the specifier is either bare or URL-like that is special.
    bool specifier_is_null_or_url_like_that_is_special { false };
};

class Window final
    : public DOM::EventTarget
    , public GlobalEventHandlers
    , public WindowEventHandlers
    , public WindowOrWorkerGlobalScopeMixin
    , public UniversalGlobalScopeMixin
    , public Bindings::WindowGlobalMixin {
    WEB_PLATFORM_OBJECT(Window, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(Window);

public:
    [[nodiscard]] static GC::Ref<Window> create(JS::Realm&);

    ~Window();

    using UniversalGlobalScopeMixin::atob;
    using UniversalGlobalScopeMixin::btoa;
    using UniversalGlobalScopeMixin::queue_microtask;
    using UniversalGlobalScopeMixin::structured_clone;
    using WindowOrWorkerGlobalScopeMixin::clear_interval;
    using WindowOrWorkerGlobalScopeMixin::clear_timeout;
    using WindowOrWorkerGlobalScopeMixin::create_image_bitmap;
    using WindowOrWorkerGlobalScopeMixin::fetch;
    using WindowOrWorkerGlobalScopeMixin::report_error;
    using WindowOrWorkerGlobalScopeMixin::set_interval;
    using WindowOrWorkerGlobalScopeMixin::set_timeout;

    // ^DOM::EventTarget
    virtual bool dispatch_event(DOM::Event&) override;

    // ^WindowOrWorkerGlobalScopeMixin
    virtual DOM::EventTarget& this_impl() override { return *this; }
    virtual DOM::EventTarget const& this_impl() const override { return *this; }

    // ^JS::Object
    virtual JS::ThrowCompletionOr<bool> internal_set_prototype_of(JS::Object* prototype) override;

    Page& page();
    Page const& page() const;

    // https://html.spec.whatwg.org/multipage/window-object.html#concept-document-window
    DOM::Document const& associated_document() const { return *m_associated_document; }
    DOM::Document& associated_document() { return *m_associated_document; }
    void set_associated_document(DOM::Document&);

    // https://html.spec.whatwg.org/multipage/window-object.html#window-bc
    BrowsingContext const* browsing_context() const;
    BrowsingContext* browsing_context();

    GC::Ptr<Navigable> navigable() const;

    void append_resolved_module(SpecifierResolution resolution) { m_resolved_module_set.append(move(resolution)); }
    Vector<SpecifierResolution> const& resolved_module_set() const { return m_resolved_module_set; }

    WebIDL::ExceptionOr<GC::Ptr<WindowProxy>> window_open_steps(StringView url, StringView target, StringView features);

    struct OpenedWindow {
        GC::Ptr<Navigable> navigable;
        TokenizedFeature::NoOpener no_opener { TokenizedFeature::NoOpener::No };
        Navigable::WindowType window_type { Navigable::WindowType::ExistingOrNone };
    };
    WebIDL::ExceptionOr<OpenedWindow> window_open_steps_internal(StringView url, StringView target, StringView features);

    DOM::Event* current_event() { return m_current_event.ptr(); }
    DOM::Event const* current_event() const { return m_current_event.ptr(); }
    void set_current_event(DOM::Event* event);

    Optional<CSS::MediaFeatureValue> query_media_feature(CSS::MediaFeatureID) const;

    void fire_a_page_transition_event(FlyString const& event_name, bool persisted);

    WebIDL::ExceptionOr<GC::Ref<Storage>> local_storage();
    WebIDL::ExceptionOr<GC::Ref<Storage>> session_storage();

    void start_an_idle_period();

    // https://html.spec.whatwg.org/multipage/interaction.html#sticky-activation
    bool has_sticky_activation() const;

    // https://html.spec.whatwg.org/multipage/interaction.html#transient-activation
    bool has_transient_activation() const;

    // https://html.spec.whatwg.org/multipage/interaction.html#history-action-activation
    bool has_history_action_activation() const;

    WebIDL::ExceptionOr<void> initialize_web_interfaces(Badge<WindowEnvironmentSettingsObject>);

    Vector<GC::Ref<Plugin>> pdf_viewer_plugin_objects();
    Vector<GC::Ref<MimeType>> pdf_viewer_mime_type_objects();

    CrossOriginPropertyDescriptorMap const& cross_origin_property_descriptor_map() const { return m_cross_origin_property_descriptor_map; }
    CrossOriginPropertyDescriptorMap& cross_origin_property_descriptor_map() { return m_cross_origin_property_descriptor_map; }

    // JS API functions
    GC::Ref<WindowProxy> window() const;
    GC::Ref<WindowProxy> self() const;
    GC::Ref<DOM::Document const> document() const;
    String name() const;
    void set_name(String const&);
    String status() const;
    void close();
    bool closed() const;
    void set_status(String const&);
    [[nodiscard]] GC::Ref<Location> location();
    GC::Ref<History> history() const;
    GC::Ref<Navigation> navigation();
    void stop();
    void focus();
    void blur();

    // For historical reasons, the Window interface had some properties that represented the visibility of certain web browser interface elements.
    // For privacy and interoperability reasons, those properties now return values that represent whether the Window's browsing context's is popup property is true or false.
    GC::Ref<BarProp const> locationbar();
    GC::Ref<BarProp const> menubar();
    GC::Ref<BarProp const> personalbar();
    GC::Ref<BarProp const> scrollbars();
    GC::Ref<BarProp const> statusbar();
    GC::Ref<BarProp const> toolbar();

    GC::Ref<WindowProxy> frames() const;
    u32 length();
    GC::Ptr<WindowProxy const> top() const;
    GC::Ptr<WindowProxy const> opener() const;
    WebIDL::ExceptionOr<void> set_opener(JS::Value);
    GC::Ptr<WindowProxy const> parent() const;
    GC::Ptr<DOM::Element const> frame_element() const;
    WebIDL::ExceptionOr<GC::Ptr<WindowProxy>> open(Optional<String> const& url, Optional<String> const& target, Optional<String> const& features);

    [[nodiscard]] GC::Ref<Navigator> navigator();
    [[nodiscard]] GC::Ref<CloseWatcherManager> close_watcher_manager();

    void alert(String const& message = {});
    bool confirm(Optional<String> const& message);
    Optional<String> prompt(Optional<String> const& message, Optional<String> const& default_);

    WebIDL::ExceptionOr<void> post_message(JS::Value message, String const&, Vector<GC::Root<JS::Object>> const&);
    WebIDL::ExceptionOr<void> post_message(JS::Value message, WindowPostMessageOptions const&);

    Variant<GC::Root<DOM::Event>, Empty> event() const;

    [[nodiscard]] GC::Ref<CSS::CSSStyleDeclaration> get_computed_style(DOM::Element&, Optional<String> const& pseudo_element) const;

    WebIDL::ExceptionOr<GC::Ref<CSS::MediaQueryList>> match_media(String const& query);
    [[nodiscard]] GC::Ref<CSS::Screen> screen();
    [[nodiscard]] GC::Ptr<CSS::VisualViewport> visual_viewport();

    i32 inner_width() const;
    i32 inner_height() const;

    void move_to(long, long) const;
    void move_by(long, long) const;
    void resize_to(long, long) const;
    void resize_by(long, long) const;

    double scroll_x() const;
    double scroll_y() const;
    void scroll(ScrollToOptions const&);
    void scroll(double x, double y);
    void scroll_by(ScrollToOptions);
    void scroll_by(double x, double y);

    i32 screen_x() const;
    i32 screen_y() const;
    i32 outer_width() const;
    i32 outer_height() const;
    double device_pixel_ratio() const;

    AnimationFrameCallbackDriver& animation_frame_callback_driver();
    bool has_animation_frame_callbacks();

    WebIDL::UnsignedLong request_animation_frame(GC::Ref<WebIDL::CallbackType>);
    void cancel_animation_frame(WebIDL::UnsignedLong handle);

    u32 request_idle_callback(WebIDL::CallbackType&, RequestIdleCallback::IdleRequestOptions const&);
    void cancel_idle_callback(u32 handle);

    GC::Ptr<Selection::Selection> get_selection() const;

    void capture_events();
    void release_events();

    [[nodiscard]] GC::Ref<CustomElementRegistry> custom_elements();

    HighResolutionTime::DOMHighResTimeStamp last_activation_timestamp() const { return m_last_activation_timestamp; }
    void set_last_activation_timestamp(HighResolutionTime::DOMHighResTimeStamp timestamp) { m_last_activation_timestamp = timestamp; }

    void consume_user_activation();

    HighResolutionTime::DOMHighResTimeStamp last_history_action_activation_timestamp() const { return m_last_history_action_activation_timestamp; }
    void set_last_history_action_activation_timestamp(HighResolutionTime::DOMHighResTimeStamp timestamp) { m_last_history_action_activation_timestamp = timestamp; }

    void consume_history_action_user_activation();

    static void set_internals_object_exposed(bool);

    [[nodiscard]] OrderedHashMap<FlyString, GC::Ref<Navigable>> document_tree_child_navigable_target_name_property_set();

    [[nodiscard]] Vector<FlyString> supported_property_names() const override;
    [[nodiscard]] JS::Value named_item_value(FlyString const&) const override;

    bool find(String const& string);

private:
    explicit Window(JS::Realm&);

    virtual void visit_edges(Cell::Visitor&) override;
    virtual void finalize() override;

    // ^HTML::GlobalEventHandlers
    virtual GC::Ptr<DOM::EventTarget> global_event_handlers_to_event_target(FlyString const&) override { return *this; }

    // ^HTML::WindowEventHandlers
    virtual GC::Ptr<DOM::EventTarget> window_event_handlers_to_event_target() override { return *this; }

    void invoke_idle_callbacks();

    struct [[nodiscard]] NamedObjects {
        Vector<GC::Ref<Navigable>> navigables;
        Vector<GC::Ref<DOM::Element>> elements;
    };
    NamedObjects named_objects(StringView name);

    WebIDL::ExceptionOr<void> window_post_message_steps(JS::Value, WindowPostMessageOptions const&);

    // https://html.spec.whatwg.org/multipage/window-object.html#concept-document-window
    GC::Ptr<DOM::Document> m_associated_document;

    GC::Ptr<DOM::Event> m_current_event;

    // https://html.spec.whatwg.org/multipage/webappapis.html#resolved-module-set
    // A global object has a resolved module set, a set of specifier resolution records, initially empty.
    //
    // Spec-Note: The resolved module set ensures that module specifier resolution returns the same result when called
    //            multiple times with the same (referrer, specifier) pair. It does that by ensuring that import map rules
    //            that impact the specifier in its referrer's scope cannot be defined after its initial resolution. For
    //            now, only Window global objects have their module set data structures modified from the initial empty one.
    Vector<SpecifierResolution> m_resolved_module_set;

    GC::Ptr<CSS::Screen> m_screen;
    GC::Ptr<Navigator> m_navigator;
    GC::Ptr<Location> m_location;
    GC::Ptr<CloseWatcherManager> m_close_watcher_manager;

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#window-navigation-api
    GC::Ptr<Navigation> m_navigation;

    // https://html.spec.whatwg.org/multipage/custom-elements.html#custom-elements-api
    // Each Window object has an associated custom element registry (a CustomElementRegistry object).
    // It is set to a new CustomElementRegistry object when the Window object is created.
    GC::Ptr<CustomElementRegistry> m_custom_element_registry;

    GC::Ptr<AnimationFrameCallbackDriver> m_animation_frame_callback_driver;

    // https://w3c.github.io/requestidlecallback/#dfn-list-of-idle-request-callbacks
    Vector<NonnullRefPtr<IdleCallback>> m_idle_request_callbacks;
    // https://w3c.github.io/requestidlecallback/#dfn-list-of-runnable-idle-callbacks
    Vector<NonnullRefPtr<IdleCallback>> m_runnable_idle_callbacks;
    // https://w3c.github.io/requestidlecallback/#dfn-idle-callback-identifier
    u32 m_idle_callback_identifier = 0;

    // https://html.spec.whatwg.org/multipage/system-state.html#pdf-viewer-plugin-objects
    Vector<GC::Ref<Plugin>> m_pdf_viewer_plugin_objects;

    // https://html.spec.whatwg.org/multipage/system-state.html#pdf-viewer-mime-type-objects
    Vector<GC::Ref<MimeType>> m_pdf_viewer_mime_type_objects;

    // [[CrossOriginPropertyDescriptorMap]], https://html.spec.whatwg.org/multipage/browsers.html#crossoriginpropertydescriptormap
    CrossOriginPropertyDescriptorMap m_cross_origin_property_descriptor_map;

    // https://html.spec.whatwg.org/multipage/interaction.html#user-activation-data-model
    HighResolutionTime::DOMHighResTimeStamp m_last_activation_timestamp { AK::Infinity<double> };

    // https://html.spec.whatwg.org/multipage/interaction.html#last-history-action-activation-timestamp
    HighResolutionTime::DOMHighResTimeStamp m_last_history_action_activation_timestamp { AK::Infinity<double> };

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-window-status
    // When the Window object is created, the attribute must be set to the empty string. It does not do anything else.
    String m_status;

    GC::Ptr<BarProp const> m_locationbar;
    GC::Ptr<BarProp const> m_menubar;
    GC::Ptr<BarProp const> m_personalbar;
    GC::Ptr<BarProp const> m_scrollbars;
    GC::Ptr<BarProp const> m_statusbar;
    GC::Ptr<BarProp const> m_toolbar;
};

void run_animation_frame_callbacks(DOM::Document&, double now);

}
