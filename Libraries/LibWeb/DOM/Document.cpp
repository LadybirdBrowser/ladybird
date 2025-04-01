/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021-2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/Debug.h>
#include <AK/GenericLexer.h>
#include <AK/InsertionSort.h>
#include <AK/StringBuilder.h>
#include <AK/TemporaryChange.h>
#include <AK/Utf8View.h>
#include <LibCore/Timer.h>
#include <LibGC/RootVector.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibURL/Origin.h>
#include <LibURL/Parser.h>
#include <LibUnicode/Segmenter.h>
#include <LibWeb/Animations/Animation.h>
#include <LibWeb/Animations/AnimationPlaybackEvent.h>
#include <LibWeb/Animations/AnimationTimeline.h>
#include <LibWeb/Animations/DocumentTimeline.h>
#include <LibWeb/Bindings/DocumentPrototype.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/CSS/AnimationEvent.h>
#include <LibWeb/CSS/CSSAnimation.h>
#include <LibWeb/CSS/CSSImportRule.h>
#include <LibWeb/CSS/CSSTransition.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/FontFaceSet.h>
#include <LibWeb/CSS/MediaQueryList.h>
#include <LibWeb/CSS/MediaQueryListEvent.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/SelectorEngine.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleSheetIdentifier.h>
#include <LibWeb/CSS/StyleValues/ColorSchemeStyleValue.h>
#include <LibWeb/CSS/SystemColor.h>
#include <LibWeb/CSS/TransitionEvent.h>
#include <LibWeb/CSS/VisualViewport.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>
#include <LibWeb/ContentSecurityPolicy/Policy.h>
#include <LibWeb/ContentSecurityPolicy/PolicyList.h>
#include <LibWeb/Cookie/ParsedCookie.h>
#include <LibWeb/DOM/AdoptedStyleSheets.h>
#include <LibWeb/DOM/Attr.h>
#include <LibWeb/DOM/CDATASection.h>
#include <LibWeb/DOM/Comment.h>
#include <LibWeb/DOM/CustomEvent.h>
#include <LibWeb/DOM/DOMImplementation.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentFragment.h>
#include <LibWeb/DOM/DocumentObserver.h>
#include <LibWeb/DOM/DocumentType.h>
#include <LibWeb/DOM/EditingHostManager.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ElementByIdMap.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/HTMLCollection.h>
#include <LibWeb/DOM/InputEventsTarget.h>
#include <LibWeb/DOM/LiveNodeList.h>
#include <LibWeb/DOM/NodeIterator.h>
#include <LibWeb/DOM/Position.h>
#include <LibWeb/DOM/ProcessingInstruction.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/DOM/TreeWalker.h>
#include <LibWeb/DOM/Utils.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/Dump.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/FileAPI/BlobURLStore.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/BeforeUnloadEvent.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/BrowsingContextGroup.h>
#include <LibWeb/HTML/CustomElements/CustomElementDefinition.h>
#include <LibWeb/HTML/CustomElements/CustomElementReactionNames.h>
#include <LibWeb/HTML/CustomElements/CustomElementRegistry.h>
#include <LibWeb/HTML/DocumentState.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/Focus.h>
#include <LibWeb/HTML/HTMLAllCollection.h>
#include <LibWeb/HTML/HTMLAnchorElement.h>
#include <LibWeb/HTML/HTMLAreaElement.h>
#include <LibWeb/HTML/HTMLBaseElement.h>
#include <LibWeb/HTML/HTMLBodyElement.h>
#include <LibWeb/HTML/HTMLDocument.h>
#include <LibWeb/HTML/HTMLEmbedElement.h>
#include <LibWeb/HTML/HTMLFormElement.h>
#include <LibWeb/HTML/HTMLFrameSetElement.h>
#include <LibWeb/HTML/HTMLHeadElement.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/HTML/HTMLIFrameElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLLinkElement.h>
#include <LibWeb/HTML/HTMLMetaElement.h>
#include <LibWeb/HTML/HTMLObjectElement.h>
#include <LibWeb/HTML/HTMLScriptElement.h>
#include <LibWeb/HTML/HTMLStyleElement.h>
#include <LibWeb/HTML/HTMLTextAreaElement.h>
#include <LibWeb/HTML/HTMLTitleElement.h>
#include <LibWeb/HTML/HashChangeEvent.h>
#include <LibWeb/HTML/ListOfAvailableImages.h>
#include <LibWeb/HTML/Location.h>
#include <LibWeb/HTML/MessageEvent.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/Navigation.h>
#include <LibWeb/HTML/NavigationParams.h>
#include <LibWeb/HTML/Numbers.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWeb/HTML/PopStateEvent.h>
#include <LibWeb/HTML/Scripting/Agent.h>
#include <LibWeb/HTML/Scripting/ClassicScript.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/HTML/Scripting/WindowEnvironmentSettingsObject.h>
#include <LibWeb/HTML/SharedResourceRequest.h>
#include <LibWeb/HTML/Storage.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/HighResolutionTime/Performance.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/Infra/CharacterTypes.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/IntersectionObserver/IntersectionObserver.h>
#include <LibWeb/Layout/BlockFormattingContext.h>
#include <LibWeb/Layout/TreeBuilder.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/ViewportPaintable.h>
#include <LibWeb/PermissionsPolicy/AutoplayAllowlist.h>
#include <LibWeb/ResizeObserver/ResizeObserver.h>
#include <LibWeb/ResizeObserver/ResizeObserverEntry.h>
#include <LibWeb/SVG/SVGDecodedImageData.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGStyleElement.h>
#include <LibWeb/SVG/SVGTitleElement.h>
#include <LibWeb/Selection/Selection.h>
#include <LibWeb/UIEvents/CompositionEvent.h>
#include <LibWeb/UIEvents/EventNames.h>
#include <LibWeb/UIEvents/FocusEvent.h>
#include <LibWeb/UIEvents/KeyboardEvent.h>
#include <LibWeb/UIEvents/MouseEvent.h>
#include <LibWeb/UIEvents/TextEvent.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(Document);

// https://html.spec.whatwg.org/multipage/origin.html#obtain-browsing-context-navigation
static GC::Ref<HTML::BrowsingContext> obtain_a_browsing_context_to_use_for_a_navigation_response(HTML::NavigationParams const& navigation_params)
{
    // 1. Let browsingContext be navigationParams's navigable's active browsing context.
    auto& browsing_context = *navigation_params.navigable->active_browsing_context();

    // 2. If browsingContext is not a top-level browsing context, return browsingContext.
    if (!browsing_context.is_top_level())
        return browsing_context;

    // 3. Let coopEnforcementResult be navigationParams's COOP enforcement result.
    auto& coop_enforcement_result = navigation_params.coop_enforcement_result;

    // 4. Let swapGroup be coopEnforcementResult's needs a browsing context group switch.
    auto swap_group = coop_enforcement_result.needs_a_browsing_context_group_switch;

    // 5. Let sourceOrigin be browsingContext's active document's origin.
    auto& source_origin = browsing_context.active_document()->origin();

    // 6. Let destinationOrigin be navigationParams's origin.
    auto& destination_origin = navigation_params.origin;

    // 7. If sourceOrigin is not same site with destinationOrigin:
    if (!source_origin.is_same_site(destination_origin)) {
        // FIXME: 1. If either of sourceOrigin or destinationOrigin have a scheme that is not an HTTP(S) scheme
        //    and the user agent considers it necessary for sourceOrigin and destinationOrigin to be
        //    isolated from each other (for implementation-defined reasons), optionally set swapGroup to true.

        // FIXME: 2. If navigationParams's user involvement is "browser UI", optionally set swapGroup to true.
    }

    // FIXME: 8. If browsingContext's group's browsing context set's size is 1, optionally set swapGroup to true.

    // 9. If swapGroup is false, then:
    if (!swap_group) {
        // 1. If coopEnforcementResult's would need a browsing context group switch due to report-only is true,
        //    set browsingContext's virtual browsing context group ID to a new unique identifier.
        if (coop_enforcement_result.would_need_a_browsing_context_group_switch_due_to_report_only) {
            // FIXME: set browsingContext's virtual browsing context group ID to a new unique identifier.
        }

        // 2. Return browsingContext.
        return browsing_context;
    }

    // 10. Let newBrowsingContext be the first return value of creating a new top-level browsing context and document.
    auto browsing_context_and_document = MUST(HTML::create_a_new_top_level_browsing_context_and_document(browsing_context.page()));
    auto new_browsing_context = browsing_context_and_document.browsing_context;

    // 11. Let navigationCOOP be navigationParams's cross-origin opener policy.
    auto navigation_coop = navigation_params.opener_policy;

    // FIXME: 12. If navigationCOOP's value is "same-origin-plus-COEP", then set newBrowsingContext's group's cross-origin
    //     isolation mode to either "logical" or "concrete". The choice of which is implementation-defined.

    // 13. Let sandboxFlags be a clone of navigationParams's final sandboxing flag set.
    auto sandbox_flags = navigation_params.final_sandboxing_flag_set;

    // 14. If sandboxFlags is not empty, then:
    if (!is_empty(sandbox_flags)) {
        // 1. Assert: navigationCOOP's value is "unsafe-none".
        VERIFY(navigation_coop.value == HTML::OpenerPolicyValue::UnsafeNone);

        // 2. Assert: newBrowsingContext's popup sandboxing flag set is empty.
        VERIFY(is_empty(new_browsing_context->popup_sandboxing_flag_set()));

        // 3. Set newBrowsingContext's popup sandboxing flag set to sandboxFlags.
        new_browsing_context->set_popup_sandboxing_flag_set(sandbox_flags);
    }

    // 15. Return newBrowsingContext.
    return new_browsing_context;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#initialise-the-document-object
WebIDL::ExceptionOr<GC::Ref<Document>> Document::create_and_initialize(Type type, String content_type, HTML::NavigationParams const& navigation_params)
{
    // 1. Let browsingContext be the result of obtaining a browsing context to use for a navigation response given navigationParams.
    auto browsing_context = obtain_a_browsing_context_to_use_for_a_navigation_response(navigation_params);

    // FIXME: 2. Let permissionsPolicy be the result of creating a permissions policy from a response given navigationParams's navigable's container, navigationParams's origin, and navigationParams's response.

    // 3. Let creationURL be navigationParams's response's URL.
    auto creation_url = navigation_params.response->url();

    // 4. If navigationParams's request is non-null, then set creationURL to navigationParams's request's current URL.
    if (navigation_params.request) {
        creation_url = navigation_params.request->current_url();
    }

    // 5. Let window be null.
    GC::Ptr<HTML::Window> window;

    // 6. If browsingContext's active document's is initial about:blank is true,
    //    and browsingContext's active document's origin is same origin-domain with navigationParams's origin,
    //    then set window to browsingContext's active window.
    // FIXME: still_on_its_initial_about_blank_document() is not in the spec anymore.
    //        However, replacing this with the spec-mandated is_initial_about_blank() results in the browsing context
    //        holding an incorrect active document for the replace from initial about:blank to the real document.
    //        See #22293 for more details.
    if (false
        && (browsing_context->active_document() && browsing_context->active_document()->origin().is_same_origin(navigation_params.origin))) {
        window = browsing_context->active_window();
    }

    // 7. Otherwise:
    else {
        // FIXME: 1. Let oacHeader be the result of getting a structured field value given `Origin-Agent-Cluster` and "item" from response's header list.

        // FIXME: 2. Let requestsOAC be true if oacHeader is not null and oacHeader[0] is the boolean true; otherwise false.
        [[maybe_unused]] auto requests_oac = false;

        // FIXME: 3. If navigationParams's reserved environment is a non-secure context, then set requestsOAC to false.

        // FIXME: 4. Let agent be the result of obtaining a similar-origin window agent given navigationParams's origin, browsingContext's group, and requestsOAC.

        // 5. Let realm execution context be the result of creating a new JavaScript realm given agent and the following customizations:
        auto realm_execution_context = Bindings::create_a_new_javascript_realm(
            Bindings::main_thread_vm(),
            [&](JS::Realm& realm) -> JS::Object* {
                // - For the global object, create a new Window object.
                window = HTML::Window::create(realm);
                return window;
            },
            [&](JS::Realm&) -> JS::Object* {
                // - For the global this binding, use browsingContext's WindowProxy object.
                return browsing_context->window_proxy();
            });

        // 6. Set window to the global object of realmExecutionContext's Realm component.
        window = as<HTML::Window>(realm_execution_context->realm->global_object());

        // 7. Let topLevelCreationURL be creationURL.
        auto top_level_creation_url = creation_url;

        // 8. Let topLevelOrigin be navigationParams's origin.
        auto top_level_origin = navigation_params.origin;

        // 9. If navigable's container is not null, then:
        if (navigation_params.navigable->container()) {
            // 1. Let parentEnvironment be navigable's container's relevant settings object.
            auto& parent_environment = HTML::relevant_settings_object(*navigation_params.navigable->container());

            // 2. Set topLevelCreationURL to parentEnvironment's top-level creation URL.
            top_level_creation_url = parent_environment.top_level_creation_url;

            // 3. Set topLevelOrigin to parentEnvironment's top-level origin.
            top_level_origin = parent_environment.top_level_origin;
        }

        // 10. Set up a window environment settings object with creationURL, realm execution context,
        //    navigationParams's reserved environment, topLevelCreationURL, and topLevelOrigin.

        // FIXME: Why do we assume `creation_url` is non-empty here? Is this a spec bug?
        // FIXME: Why do we assume `top_level_creation_url` is non-empty here? Is this a spec bug?
        HTML::WindowEnvironmentSettingsObject::setup(
            browsing_context->page(),
            creation_url.value(),
            move(realm_execution_context),
            navigation_params.reserved_environment,
            top_level_creation_url.value(),
            top_level_origin);
    }

    // 8. Let loadTimingInfo be a new document load timing info with its navigation start time set to navigationParams's response's timing info's start time.
    DOM::DocumentLoadTimingInfo load_timing_info;
    // AD-HOC: The response object no longer has an associated timing info object. For now, we use response's non-standard response time property,
    //         which represents the time that the time that the response object was created.
    auto response_creation_time = navigation_params.response->response_time().nanoseconds() / 1e6;
    load_timing_info.navigation_start_time = HighResolutionTime::coarsen_time(response_creation_time, HTML::relevant_settings_object(*window).cross_origin_isolated_capability() == HTML::CanUseCrossOriginIsolatedAPIs::Yes);

    // 9. Let document be a new Document, with
    //    type: type
    //    content type: contentType
    //    origin: navigationParams's origin
    //    browsing context: browsingContext
    //    policy container: navigationParams's policy container
    //    FIXME: permissions policy: permissionsPolicy
    //    active sandboxing flag set: navigationParams's final sandboxing flag set
    //    FIXME: opener policy: navigationParams's opener policy
    //    load timing info: loadTimingInfo
    //    FIXME: was created via cross-origin redirects: navigationParams's response's has cross-origin redirects
    //    during-loading navigation ID for WebDriver BiDi: navigationParams's id
    //    URL: creationURL
    //    current document readiness: "loading"
    //    about base URL: navigationParams's about base URL
    //    allow declarative shadow roots: true
    auto document = HTML::HTMLDocument::create(window->realm());
    document->m_type = type;
    document->m_content_type = move(content_type);
    document->set_origin(navigation_params.origin);
    document->set_browsing_context(browsing_context);
    document->m_policy_container = navigation_params.policy_container;
    document->m_active_sandboxing_flag_set = navigation_params.final_sandboxing_flag_set;
    document->m_navigation_id = navigation_params.id;
    document->set_load_timing_info(load_timing_info);
    document->set_url(*creation_url);
    document->m_readiness = HTML::DocumentReadyState::Loading;
    document->m_about_base_url = navigation_params.about_base_url;
    document->set_allow_declarative_shadow_roots(true);

    document->m_window = window;

    // NOTE: Non-standard: Pull out the Last-Modified header for use in the lastModified property.
    if (auto maybe_last_modified = navigation_params.response->header_list()->get("Last-Modified"sv.bytes()); maybe_last_modified.has_value())
        document->m_last_modified = Core::DateTime::parse("%a, %d %b %Y %H:%M:%S %Z"sv, maybe_last_modified.value());

    // NOTE: Non-standard: Pull out the Content-Language header to determine the document's language.
    if (auto maybe_http_content_language = navigation_params.response->header_list()->get("Content-Language"sv.bytes()); maybe_http_content_language.has_value()) {
        if (auto maybe_content_language = String::from_utf8(maybe_http_content_language.value()); !maybe_content_language.is_error())
            document->m_http_content_language = maybe_content_language.release_value();
    }

    // 10. Set window's associated Document to document.
    window->set_associated_document(*document);

    // 11. Run CSP initialization for a Document given document.
    document->run_csp_initialization();

    // 12. If navigationParams's request is non-null, then:
    if (navigation_params.request) {
        // 1. Set document's referrer to the empty string.
        document->m_referrer = String {};

        // 2. Let referrer be navigationParams's request's referrer.
        auto const& referrer = navigation_params.request->referrer();

        // 3. If referrer is a URL record, then set document's referrer to the serialization of referrer.
        if (referrer.has<URL::URL>()) {
            document->m_referrer = referrer.get<URL::URL>().serialize();
        }
    }

    // FIXME: 13: If navigationParams's fetch controller is not null, then:

    // FIXME: 14. Create the navigation timing entry for document, with navigationParams's response's timing info, redirectCount, navigationParams's navigation timing type, and
    //            navigationParams's response's service worker timing info.

    // 15. If navigationParams's response has a `Refresh` header, then:
    if (auto maybe_refresh = navigation_params.response->header_list()->get("Refresh"sv.bytes()); maybe_refresh.has_value()) {
        // 1. Let value be the isomorphic decoding of the value of the header.
        auto value = Infra::isomorphic_decode(maybe_refresh.value());

        // 2. Run the shared declarative refresh steps with document and value.
        document->shared_declarative_refresh_steps(value, nullptr);
    }

    // FIXME: 16. If navigationParams's commit early hints is not null, then call navigationParams's commit early hints with document.

    // FIXME: 17. Process link headers given document, navigationParams's response, and "pre-media".

    // 18. Return document.
    return document;
}

WebIDL::ExceptionOr<GC::Ref<Document>> Document::construct_impl(JS::Realm& realm)
{
    return Document::create(realm);
}

GC::Ref<Document> Document::create(JS::Realm& realm, URL::URL const& url)
{
    return realm.create<Document>(realm, url);
}

GC::Ref<Document> Document::create_for_fragment_parsing(JS::Realm& realm)
{
    return realm.create<Document>(realm, URL::about_blank(), TemporaryDocumentForFragmentParsing::Yes);
}

Document::Document(JS::Realm& realm, const URL::URL& url, TemporaryDocumentForFragmentParsing temporary_document_for_fragment_parsing)
    : ParentNode(realm, *this, NodeType::DOCUMENT_NODE)
    , m_page(Bindings::principal_host_defined_page(realm))
    , m_style_computer(make<CSS::StyleComputer>(*this))
    , m_url(url)
    , m_temporary_document_for_fragment_parsing(temporary_document_for_fragment_parsing)
    , m_editing_host_manager(EditingHostManager::create(realm, *this))
{
    m_legacy_platform_object_flags = PlatformObject::LegacyPlatformObjectFlags {
        .supports_named_properties = true,
        .has_legacy_override_built_ins_interface_extended_attribute = true,
    };

    m_cursor_blink_timer = Core::Timer::create_repeating(500, [this] {
        auto cursor_position = this->cursor_position();
        if (!cursor_position)
            return;

        auto node = cursor_position->node();
        if (!node)
            return;

        auto navigable = this->navigable();
        if (!navigable || !navigable->is_focused())
            return;

        node->document().update_layout(UpdateLayoutReason::CursorBlinkTimer);

        if (node->paintable()) {
            m_cursor_blink_state = !m_cursor_blink_state;
            node->paintable()->set_needs_display();
        }
    });

    HTML::main_thread_event_loop().register_document({}, *this);
}

Document::~Document()
{
    HTML::main_thread_event_loop().unregister_document({}, *this);
}

void Document::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Document);
    Bindings::DocumentPrototype::define_unforgeable_attributes(realm, *this);

    m_selection = realm.create<Selection::Selection>(realm, *this);

    m_list_of_available_images = realm.create<HTML::ListOfAvailableImages>();

    page().client().page_did_create_new_document(*this);
}

// https://html.spec.whatwg.org/multipage/document-lifecycle.html#populate-with-html/head/body
WebIDL::ExceptionOr<void> Document::populate_with_html_head_and_body()
{
    // 1. Let html be the result of creating an element given document, "html", and the HTML namespace.
    auto html = TRY(DOM::create_element(*this, HTML::TagNames::html, Namespace::HTML));

    // 2. Let head be the result of creating an element given document, "head", and the HTML namespace.
    auto head = TRY(DOM::create_element(*this, HTML::TagNames::head, Namespace::HTML));

    // 3. Let body be the result of creating an element given document, "body", and the HTML namespace.
    auto body = TRY(DOM::create_element(*this, HTML::TagNames::body, Namespace::HTML));

    // 4. Append html to document.
    TRY(append_child(html));

    // 5. Append head to html.
    TRY(html->append_child(head));

    // 6. Append body to html.
    TRY(html->append_child(body));

    return {};
}

void Document::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_page);
    visitor.visit(m_window);
    visitor.visit(m_layout_root);
    visitor.visit(m_style_sheets);
    visitor.visit(m_hovered_node);
    visitor.visit(m_inspected_node);
    visitor.visit(m_highlighted_node);
    visitor.visit(m_active_favicon);
    visitor.visit(m_focused_element);
    visitor.visit(m_active_element);
    visitor.visit(m_target_element);
    visitor.visit(m_implementation);
    visitor.visit(m_current_script);
    visitor.visit(m_associated_inert_template_document);
    visitor.visit(m_appropriate_template_contents_owner_document);
    visitor.visit(m_pending_parsing_blocking_script);
    visitor.visit(m_history);

    visitor.visit(m_browsing_context);

    visitor.visit(m_applets);
    visitor.visit(m_anchors);
    visitor.visit(m_images);
    visitor.visit(m_embeds);
    visitor.visit(m_links);
    visitor.visit(m_forms);
    visitor.visit(m_scripts);
    visitor.visit(m_all);
    visitor.visit(m_fonts);
    visitor.visit(m_selection);
    visitor.visit(m_first_base_element_with_href_in_tree_order);
    visitor.visit(m_first_base_element_with_target_in_tree_order);
    visitor.visit(m_parser);
    visitor.visit(m_lazy_load_intersection_observer);
    visitor.visit(m_visual_viewport);
    visitor.visit(m_latest_entry);
    visitor.visit(m_default_timeline);
    visitor.visit(m_scripts_to_execute_when_parsing_has_finished);
    visitor.visit(m_scripts_to_execute_in_order_as_soon_as_possible);
    visitor.visit(m_scripts_to_execute_as_soon_as_possible);
    visitor.visit(m_node_iterators);
    visitor.visit(m_document_observers);
    visitor.visit(m_document_observers_being_notified);
    visitor.visit(m_pending_scroll_event_targets);
    visitor.visit(m_pending_scrollend_event_targets);
    visitor.visit(m_resize_observers);

    visitor.visit(m_shared_resource_requests);

    visitor.visit(m_associated_animation_timelines);
    visitor.visit(m_list_of_available_images);

    for (auto* form_associated_element : m_form_associated_elements_with_form_attribute)
        visitor.visit(form_associated_element->form_associated_element_to_html_element());

    visitor.visit(m_potentially_named_elements);

    for (auto& event : m_pending_animation_event_queue) {
        visitor.visit(event.event);
        visitor.visit(event.animation);
        visitor.visit(event.target);
    }

    visitor.visit(m_adopted_style_sheets);

    visitor.visit(m_shadow_roots);

    visitor.visit(m_top_layer_elements);
    visitor.visit(m_top_layer_pending_removals);
    visitor.visit(m_showing_auto_popover_list);
    visitor.visit(m_showing_hint_popover_list);
    visitor.visit(m_console_client);
    visitor.visit(m_editing_host_manager);
    visitor.visit(m_local_storage_holder);
    visitor.visit(m_session_storage_holder);
    visitor.visit(m_render_blocking_elements);
    visitor.visit(m_policy_container);
}

// https://w3c.github.io/selection-api/#dom-document-getselection
GC::Ptr<Selection::Selection> Document::get_selection() const
{
    // The method must return the selection associated with this if this has an associated browsing context,
    // and it must return null otherwise.
    if (!browsing_context())
        return {};
    return m_selection;
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-document-write
WebIDL::ExceptionOr<void> Document::write(Vector<String> const& text)
{
    // The document.write(...text) method steps are to run the document write steps with this, text, false, and "Document write".
    return run_the_document_write_steps(text, AddLineFeed::No, TrustedTypes::InjectionSink::DocumentWrite);
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-document-writeln
WebIDL::ExceptionOr<void> Document::writeln(Vector<String> const& text)
{
    // The document.writeln(...text) method steps are to run the document write steps with this, text, true, and "Document writeln".
    return run_the_document_write_steps(text, AddLineFeed::Yes, TrustedTypes::InjectionSink::DocumentWriteln);
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#document-write-steps
WebIDL::ExceptionOr<void> Document::run_the_document_write_steps(Vector<String> const& text, AddLineFeed line_feed, TrustedTypes::InjectionSink sink)
{
    // 1. Let string be the empty string.
    StringBuilder string;

    // 2. Let isTrusted be false if text contains a string; otherwise true.
    // FIXME: We currently only accept strings. Revisit this once we support the TrustedHTML type.
    auto is_trusted = true;

    // 3. For each value of text:
    for (auto const& value : text) {
        // FIXME: 1. If value is a TrustedHTML object, then append value's associated data to string.

        // 2. Otherwise, append value to string.
        string.append(value);
    }

    // FIXME: 4. If isTrusted is false, set string to the result of invoking the Get Trusted Type compliant string algorithm
    //    with TrustedHTML, this's relevant global object, string, sink, and "script".
    (void)is_trusted;
    (void)sink;

    // 5. If lineFeed is true, append U+000A LINE FEED to string.
    if (line_feed == AddLineFeed::Yes)
        string.append('\n');

    // 6. If document is an XML document, then throw an "InvalidStateError" DOMException.
    if (m_type == Type::XML)
        return WebIDL::InvalidStateError::create(realm(), "write() called on XML document."_string);

    // 7. If document's throw-on-dynamic-markup-insertion counter is greater than 0, then throw an "InvalidStateError" DOMException.
    if (m_throw_on_dynamic_markup_insertion_counter > 0)
        return WebIDL::InvalidStateError::create(realm(), "throw-on-dynamic-markup-insertion-counter greater than zero."_string);

    // 8. If document's active parser was aborted is true, then return.
    if (m_active_parser_was_aborted)
        return {};

    // 9. If the insertion point is undefined, then:
    if (!(m_parser && m_parser->tokenizer().is_insertion_point_defined())) {
        // 1. If document's unload counter is greater than 0 or document's ignore-destructive-writes counter is greater than 0, then return.
        if (m_unload_counter > 0 || m_ignore_destructive_writes_counter > 0)
            return {};

        // 2. Run the document open steps with document.
        TRY(open());
    }

    // 10. Insert string into the input stream just before the insertion point.
    m_parser->tokenizer().insert_input_at_insertion_point(string.string_view());

    // 11. If document's pending parsing-blocking script is null, then have the HTML parser process string, one code
    //     point at a time, processing resulting tokens as they are emitted, and stopping when the tokenizer reaches
    //     the insertion point or when the processing of the tokenizer is aborted by the tree construction stage (this
    //     can happen if a script end tag token is emitted by the tokenizer).
    if (!pending_parsing_blocking_script())
        m_parser->run(HTML::HTMLTokenizer::StopAtInsertionPoint::Yes);

    return {};
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-document-open
WebIDL::ExceptionOr<Document*> Document::open(Optional<String> const&, Optional<String> const&)
{
    // If document belongs to a child navigable, we need to make sure its initial navigation is done,
    // because subsequent steps will modify "initial about:blank" to false, which would cause
    // initial navigation to fail in case it was "about:blank".
    if (auto navigable = this->navigable(); navigable && navigable->container() && !navigable->container()->content_navigable_has_session_history_entry_and_ready_for_navigation()) {
        HTML::main_thread_event_loop().spin_processing_tasks_with_source_until(HTML::Task::Source::NavigationAndTraversal, GC::create_function(heap(), [navigable_container = navigable->container()] {
            return navigable_container->content_navigable_has_session_history_entry_and_ready_for_navigation();
        }));
    }

    // 1. If document is an XML document, then throw an "InvalidStateError" DOMException exception.
    if (m_type == Type::XML)
        return WebIDL::InvalidStateError::create(realm(), "open() called on XML document."_string);

    // 2. If document's throw-on-dynamic-markup-insertion counter is greater than 0, then throw an "InvalidStateError" DOMException.
    if (m_throw_on_dynamic_markup_insertion_counter > 0)
        return WebIDL::InvalidStateError::create(realm(), "throw-on-dynamic-markup-insertion-counter greater than zero."_string);

    // FIXME: 3. Let entryDocument be the entry global object's associated Document.
    auto& entry_document = *this;

    // 4. If document's origin is not same origin to entryDocument's origin, then throw a "SecurityError" DOMException.
    if (origin() != entry_document.origin())
        return WebIDL::SecurityError::create(realm(), "Document.origin() not the same as entryDocument's."_string);

    // 5. If document has an active parser whose script nesting level is greater than 0, then return document.
    if (m_parser && m_parser->script_nesting_level() > 0)
        return this;

    // 6. Similarly, if document's unload counter is greater than 0, then return document.
    if (m_unload_counter > 0)
        return this;

    // 7. If document's active parser was aborted is true, then return document.
    if (m_active_parser_was_aborted)
        return this;

    // FIXME: 8. If document's browsing context is non-null and there is an existing attempt to navigate document's browsing context, then stop document loading given document.

    // FIXME: 9. For each shadow-including inclusive descendant node of document, erase all event listeners and handlers given node.

    // FIXME 10. If document is the associated Document of document's relevant global object, then erase all event listeners and handlers given document's relevant global object.

    // 11. Replace all with null within document, without firing any mutation events.
    replace_all(nullptr);

    // https://w3c.github.io/editing/docs/execCommand/#state-override
    // When document.open() is called and a document's singleton objects are all replaced by new instances of those
    // objects, editing state associated with that document (including the CSS styling flag, default single-line
    // container name, and any state overrides or value overrides) must be reset.
    set_css_styling_flag(false);
    set_default_single_line_container_name(HTML::TagNames::div);
    reset_command_state_overrides();
    reset_command_value_overrides();

    // 12. If document is fully active, then:
    if (is_fully_active()) {
        // 1. Let newURL be a copy of entryDocument's URL.
        auto new_url = entry_document.url();
        // 2. If entryDocument is not document, then set newURL's fragment to null.
        if (&entry_document != this)
            new_url.set_fragment({});

        // FIXME: 3. Run the URL and history update steps with document and newURL.
    }

    // 13. Set document's is initial about:blank to false.
    set_is_initial_about_blank(false);

    // FIXME: 14. If document's iframe load in progress flag is set, then set document's mute iframe load flag.

    // 15. Set document to no-quirks mode.
    set_quirks_mode(QuirksMode::No);

    // 16. Create a new HTML parser and associate it with document. This is a script-created parser (meaning that it can be closed by the document.open() and document.close() methods, and that the tokenizer will wait for an explicit call to document.close() before emitting an end-of-file token). The encoding confidence is irrelevant.
    m_parser = HTML::HTMLParser::create_for_scripting(*this);

    // 17. Set the insertion point to point at just before the end of the input stream (which at this point will be empty).
    m_parser->tokenizer().update_insertion_point();

    // 18. Update the current document readiness of document to "loading".
    update_readiness(HTML::DocumentReadyState::Loading);

    // 19. Return document.
    return this;
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-document-open-window
WebIDL::ExceptionOr<GC::Ptr<HTML::WindowProxy>> Document::open(StringView url, StringView name, StringView features)
{
    // 1. If this is not fully active, then throw an "InvalidAccessError" DOMException exception.
    if (!is_fully_active())
        return WebIDL::InvalidAccessError::create(realm(), "Cannot perform open on a document that isn't fully active."_string);

    // 2. Return the result of running the window open steps with url, name, and features.
    return window()->window_open_steps(url, name, features);
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#closing-the-input-stream
WebIDL::ExceptionOr<void> Document::close()
{
    // 1. If document is an XML document, then throw an "InvalidStateError" DOMException exception.
    if (m_type == Type::XML)
        return WebIDL::InvalidStateError::create(realm(), "close() called on XML document."_string);

    // 2. If document's throw-on-dynamic-markup-insertion counter is greater than 0, then throw an "InvalidStateError" DOMException.
    if (m_throw_on_dynamic_markup_insertion_counter > 0)
        return WebIDL::InvalidStateError::create(realm(), "throw-on-dynamic-markup-insertion-counter greater than zero."_string);

    // 3. If there is no script-created parser associated with the document, then return.
    if (!m_parser)
        return {};

    // 4. Insert an explicit "EOF" character at the end of the parser's input stream.
    m_parser->tokenizer().insert_eof();

    // 5. If there is a pending parsing-blocking script, then return.
    if (pending_parsing_blocking_script())
        return {};

    // 6. Run the tokenizer, processing resulting tokens as they are emitted, and stopping when the tokenizer reaches the explicit "EOF" character or spins the event loop.
    m_parser->run();

    // AD-HOC: This ensures that a load event is fired if the node navigable's container is an iframe.
    completely_finish_loading();

    return {};
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-document-defaultview
GC::Ptr<HTML::WindowProxy> Document::default_view()
{
    // If this's browsing context is null, then return null.
    if (!browsing_context())
        return {};

    // 2. Return this's browsing context's WindowProxy object.
    return browsing_context()->window_proxy();
}

GC::Ptr<HTML::WindowProxy const> Document::default_view() const
{
    return const_cast<Document*>(this)->default_view();
}

URL::Origin const& Document::origin() const
{
    return m_origin;
}

void Document::set_origin(URL::Origin const& origin)
{
    m_origin = origin;
}

void Document::schedule_style_update()
{
    if (!browsing_context())
        return;

    // NOTE: Update of the style is a step in HTML event loop processing.
    HTML::main_thread_event_loop().schedule();
}

void Document::schedule_layout_update()
{
    if (!browsing_context())
        return;

    // NOTE: Update of the layout is a step in HTML event loop processing.
    HTML::main_thread_event_loop().schedule();
}

bool Document::is_child_allowed(Node const& node) const
{
    switch (node.type()) {
    case NodeType::DOCUMENT_NODE:
    case NodeType::TEXT_NODE:
        return false;
    case NodeType::COMMENT_NODE:
        return true;
    case NodeType::DOCUMENT_TYPE_NODE:
        return !first_child_of_type<DocumentType>();
    case NodeType::ELEMENT_NODE:
        return !first_child_of_type<Element>();
    default:
        return false;
    }
}

Element* Document::document_element()
{
    return first_child_of_type<Element>();
}

Element const* Document::document_element() const
{
    return first_child_of_type<Element>();
}

// https://html.spec.whatwg.org/multipage/dom.html#the-html-element-2
HTML::HTMLHtmlElement* Document::html_element()
{
    // The html element of a document is its document element, if it's an html element, and null otherwise.
    auto* html = document_element();
    if (is<HTML::HTMLHtmlElement>(html))
        return as<HTML::HTMLHtmlElement>(html);
    return nullptr;
}

// https://html.spec.whatwg.org/multipage/dom.html#the-head-element-2
HTML::HTMLHeadElement* Document::head()
{
    // The head element of a document is the first head element that is a child of the html element, if there is one,
    // or null otherwise.
    auto* html = html_element();
    if (!html)
        return nullptr;
    return html->first_child_of_type<HTML::HTMLHeadElement>();
}

// https://html.spec.whatwg.org/multipage/dom.html#the-title-element-2
GC::Ptr<HTML::HTMLTitleElement> Document::title_element()
{
    // The title element of a document is the first title element in the document (in tree order), if there is one, or
    // null otherwise.
    GC::Ptr<HTML::HTMLTitleElement> title_element = nullptr;

    for_each_in_subtree_of_type<HTML::HTMLTitleElement>([&](auto& title_element_in_tree) {
        title_element = title_element_in_tree;
        return TraversalDecision::Break;
    });

    return title_element;
}

// https://html.spec.whatwg.org/multipage/dom.html#dom-document-dir
StringView Document::dir() const
{
    // The dir IDL attribute on Document objects must reflect the dir content attribute of the html
    // element, if any, limited to only known values. If there is no such element, then the
    // attribute must return the empty string and do nothing on setting.
    if (auto html = html_element())
        return html->dir();

    return ""sv;
}

// https://html.spec.whatwg.org/multipage/dom.html#dom-document-dir
void Document::set_dir(String const& dir)
{
    // The dir IDL attribute on Document objects must reflect the dir content attribute of the html
    // element, if any, limited to only known values. If there is no such element, then the
    // attribute must return the empty string and do nothing on setting.
    if (auto html = html_element())
        html->set_dir(dir);
}

// https://html.spec.whatwg.org/multipage/dom.html#the-body-element-2
HTML::HTMLElement* Document::body()
{
    // The body element of a document is the first of the html element's children that is either
    // a body element or a frameset element, or null if there is no such element.
    auto* html = html_element();
    if (!html)
        return nullptr;
    for (auto* child = html->first_child(); child; child = child->next_sibling()) {
        if (is<HTML::HTMLBodyElement>(*child) || is<HTML::HTMLFrameSetElement>(*child))
            return static_cast<HTML::HTMLElement*>(child);
    }
    return nullptr;
}

// https://html.spec.whatwg.org/multipage/dom.html#dom-document-body
WebIDL::ExceptionOr<void> Document::set_body(HTML::HTMLElement* new_body)
{
    if (!is<HTML::HTMLBodyElement>(new_body) && !is<HTML::HTMLFrameSetElement>(new_body))
        return WebIDL::HierarchyRequestError::create(realm(), "Invalid document body element, must be 'body' or 'frameset'"_string);

    auto* existing_body = body();
    if (existing_body) {
        (void)TRY(existing_body->parent()->replace_child(*new_body, *existing_body));
        return {};
    }

    auto* document_element = this->document_element();
    if (!document_element)
        return WebIDL::HierarchyRequestError::create(realm(), "Missing document element"_string);

    (void)TRY(document_element->append_child(*new_body));
    return {};
}

// https://html.spec.whatwg.org/multipage/dom.html#document.title
String Document::title() const
{
    String value;

    // 1. If the document element is an SVG svg element, then let value be the child text content of the first SVG title
    //    element that is a child of the document element.
    if (auto const* document_element = this->document_element(); is<SVG::SVGElement>(document_element)) {
        if (auto const* title_element = document_element->first_child_of_type<SVG::SVGTitleElement>())
            value = title_element->child_text_content();
    }

    // 2. Otherwise, let value be the child text content of the title element, or the empty string if the title element
    //    is null.
    else if (auto title_element = this->title_element()) {
        value = title_element->text_content().value_or(String {});
    }

    // 3. Strip and collapse ASCII whitespace in value.
    auto title = Infra::strip_and_collapse_whitespace(value).release_value_but_fixme_should_propagate_errors();

    // 4. Return value.
    return title;
}

// https://html.spec.whatwg.org/multipage/dom.html#document.title
WebIDL::ExceptionOr<void> Document::set_title(String const& title)
{
    auto* document_element = this->document_element();

    // -> If the document element is an SVG svg element
    if (is<SVG::SVGElement>(document_element)) {
        GC::Ptr<Element> element;

        // 1. If there is an SVG title element that is a child of the document element, let element be the first such
        //    element.
        if (auto* title_element = document_element->first_child_of_type<SVG::SVGTitleElement>()) {
            element = title_element;
        }
        // 2. Otherwise:
        else {
            // 1. Let element be the result of creating an element given the document element's node document, "title",
            //    and the SVG namespace.
            element = TRY(DOM::create_element(*this, HTML::TagNames::title, Namespace::SVG));

            // 2. Insert element as the first child of the document element.
            document_element->insert_before(*element, document_element->first_child());
        }

        // 3. String replace all with the given value within element.
        element->string_replace_all(title);
    }

    // -> If the document element is in the HTML namespace
    else if (document_element && document_element->namespace_uri() == Namespace::HTML) {
        auto title_element = this->title_element();
        auto* head_element = this->head();

        // 1. If the title element is null and the head element is null, then return.
        if (title_element == nullptr && head_element == nullptr)
            return {};

        GC::Ptr<Element> element;

        // 2. If the title element is non-null, let element be the title element.
        if (title_element) {
            element = title_element;
        }
        // 3. Otherwise:
        else {
            // 1. Let element be the result of creating an element given the document element's node document, "title",
            //    and the HTML namespace.
            element = TRY(DOM::create_element(*this, HTML::TagNames::title, Namespace::HTML));

            // 2. Append element to the head element.
            TRY(head_element->append_child(*element));
        }

        // 4. String replace all with the given value within element.
        element->string_replace_all(title);
    }

    // -> Otherwise
    else {
        // Do nothing.
        return {};
    }

    return {};
}

void Document::tear_down_layout_tree()
{
    m_layout_root = nullptr;
    m_paintable = nullptr;
    m_needs_full_layout_tree_update = true;
}

Color Document::background_color() const
{
    // CSS2 says we should use the HTML element's background color unless it's transparent...
    if (auto* html_element = this->html_element(); html_element && html_element->layout_node()) {
        auto color = html_element->layout_node()->computed_values().background_color();
        if (color.alpha())
            return color;
    }

    // ...in which case we use the BODY element's background color.
    if (auto* body_element = body(); body_element && body_element->layout_node()) {
        auto color = body_element->layout_node()->computed_values().background_color();
        return color;
    }

    // By default, the document is transparent.
    // The outermost canvas is colored by the PageHost.
    return Color::Transparent;
}

Vector<CSS::BackgroundLayerData> const* Document::background_layers() const
{
    auto* body_element = body();
    if (!body_element)
        return {};

    auto body_layout_node = body_element->layout_node();
    if (!body_layout_node)
        return {};

    return &body_layout_node->background_layers();
}

void Document::update_base_element(Badge<HTML::HTMLBaseElement>)
{
    GC::Ptr<HTML::HTMLBaseElement const> base_element_with_href = nullptr;
    GC::Ptr<HTML::HTMLBaseElement const> base_element_with_target = nullptr;

    for_each_in_subtree_of_type<HTML::HTMLBaseElement>([&base_element_with_href, &base_element_with_target](HTML::HTMLBaseElement const& base_element_in_tree) {
        if (!base_element_with_href && base_element_in_tree.has_attribute(HTML::AttributeNames::href)) {
            base_element_with_href = &base_element_in_tree;
            if (base_element_with_target)
                return TraversalDecision::Break;
        }
        if (!base_element_with_target && base_element_in_tree.has_attribute(HTML::AttributeNames::target)) {
            base_element_with_target = &base_element_in_tree;
            if (base_element_with_href)
                return TraversalDecision::Break;
        }

        return TraversalDecision::Continue;
    });

    m_first_base_element_with_href_in_tree_order = base_element_with_href;
    m_first_base_element_with_target_in_tree_order = base_element_with_target;
}

GC::Ptr<HTML::HTMLBaseElement const> Document::first_base_element_with_href_in_tree_order() const
{
    return m_first_base_element_with_href_in_tree_order;
}

GC::Ptr<HTML::HTMLBaseElement const> Document::first_base_element_with_target_in_tree_order() const
{
    return m_first_base_element_with_target_in_tree_order;
}

// https://html.spec.whatwg.org/multipage/urls-and-fetching.html#fallback-base-url
URL::URL Document::fallback_base_url() const
{
    // 1. If document is an iframe srcdoc document, then:
    if (HTML::url_matches_about_srcdoc(m_url)) {
        // 1. Assert: document's about base URL is non-null.
        VERIFY(m_about_base_url.has_value());

        // 2. Return document's about base URL.
        return m_about_base_url.value();
    }

    // 2. If document's URL matches about:blank and document's about base URL is non-null, then return document's about base URL.
    if (HTML::url_matches_about_blank(m_url) && m_about_base_url.has_value())
        return m_about_base_url.value();

    // 3. Return document's URL.
    return m_url;
}

// https://html.spec.whatwg.org/multipage/urls-and-fetching.html#document-base-url
URL::URL Document::base_url() const
{
    // 1. If there is no base element that has an href attribute in the Document, then return the Document's fallback base URL.
    auto base_element = first_base_element_with_href_in_tree_order();
    if (!base_element)
        return fallback_base_url();

    // 2. Otherwise, return the frozen base URL of the first base element in the Document that has an href attribute, in tree order.
    return base_element->frozen_base_url();
}

// https://html.spec.whatwg.org/multipage/urls-and-fetching.html#parse-a-url
Optional<URL::URL> Document::parse_url(StringView url) const
{
    // 1. Let baseURL be environment's base URL, if environment is a Document object; otherwise environment's API base URL.
    auto base_url = this->base_url();

    // 2. Return the result of applying the URL parser to url, with baseURL.
    return DOMURL::parse(url, base_url);
}

// https://html.spec.whatwg.org/multipage/urls-and-fetching.html#encoding-parsing-a-url
Optional<URL::URL> Document::encoding_parse_url(StringView url) const
{
    // 1. Let encoding be UTF-8.
    // 2. If environment is a Document object, then set encoding to environment's character encoding.
    auto encoding = encoding_or_default();

    // 3. Otherwise, if environment's relevant global object is a Window object, set encoding to environment's relevant
    //    global object's associated Document's character encoding.

    // 4. Let baseURL be environment's base URL, if environment is a Document object; otherwise environment's API base URL.
    auto base_url = this->base_url();

    // 5. Return the result of applying the URL parser to url, with baseURL and encoding.
    return DOMURL::parse(url, base_url, encoding);
}

// https://html.spec.whatwg.org/multipage/urls-and-fetching.html#encoding-parsing-and-serializing-a-url
Optional<String> Document::encoding_parse_and_serialize_url(StringView url) const
{
    // 1. Let url be the result of encoding-parsing a URL given url, relative to environment.
    auto parsed_url = encoding_parse_url(url);

    // 2. If url is failure, then return failure.
    if (!parsed_url.has_value())
        return {};

    // 3. Return the result of applying the URL serializer to url.
    return parsed_url->serialize();
}

void Document::invalidate_layout_tree(InvalidateLayoutTreeReason reason)
{
    if (m_layout_root)
        dbgln_if(UPDATE_LAYOUT_DEBUG, "DROP TREE {}", to_string(reason));
    tear_down_layout_tree();
    schedule_layout_update();
}

static void propagate_scrollbar_width_to_viewport(Element& root_element, Layout::Viewport& viewport)
{
    // https://drafts.csswg.org/css-scrollbars/#scrollbar-width
    // UAs must apply the scrollbar-color value set on the root element to the viewport.
    auto& viewport_computed_values = viewport.mutable_computed_values();
    auto& root_element_computed_values = root_element.layout_node()->computed_values();
    viewport_computed_values.set_scrollbar_width(root_element_computed_values.scrollbar_width());
}

// https://drafts.csswg.org/css-overflow-3/#overflow-propagation
static void propagate_overflow_to_viewport(Element& root_element, Layout::Viewport& viewport)
{
    // https://drafts.csswg.org/css-contain-2/#contain-property
    // Additionally, when any containments are active on either the HTML <html> or <body> elements, propagation of
    // properties from the <body> element to the initial containing block, the viewport, or the canvas background, is
    // disabled. Notably, this affects:
    // - 'overflow' and its longhands (see CSS Overflow 3 § 3.3 Overflow Viewport Propagation)
    if (root_element.is_html_html_element() && !root_element.computed_properties()->contain().is_empty())
        return;

    auto* body_element = root_element.first_child_of_type<HTML::HTMLBodyElement>();
    if (body_element && !body_element->computed_properties()->contain().is_empty())
        return;

    // UAs must apply the overflow-* values set on the root element to the viewport
    // when the root element’s display value is not none.
    auto overflow_origin_node = root_element.layout_node();
    auto& viewport_computed_values = viewport.mutable_computed_values();

    // However, when the root element is an [HTML] html element (including XML syntax for HTML)
    // whose overflow value is visible (in both axes), and that element has as a child
    // a body element whose display value is also not none,
    // user agents must instead apply the overflow-* values of the first such child element to the viewport.
    if (root_element.is_html_html_element()) {
        auto root_element_layout_node = root_element.layout_node();
        auto& root_element_computed_values = root_element_layout_node->mutable_computed_values();
        if (root_element_computed_values.overflow_x() == CSS::Overflow::Visible && root_element_computed_values.overflow_y() == CSS::Overflow::Visible) {
            auto* body_element = root_element.first_child_of_type<HTML::HTMLBodyElement>();
            if (body_element && body_element->layout_node())
                overflow_origin_node = body_element->layout_node();
        }
    }

    // NOTE: This is where we assign the chosen overflow values to the viewport.
    auto& overflow_origin_computed_values = overflow_origin_node->mutable_computed_values();
    viewport_computed_values.set_overflow_x(overflow_origin_computed_values.overflow_x());
    viewport_computed_values.set_overflow_y(overflow_origin_computed_values.overflow_y());

    // The element from which the value is propagated must then have a used overflow value of visible.
    overflow_origin_computed_values.set_overflow_x(CSS::Overflow::Visible);
    overflow_origin_computed_values.set_overflow_y(CSS::Overflow::Visible);
}

void Document::update_layout(UpdateLayoutReason reason)
{
    auto navigable = this->navigable();
    if (!navigable || navigable->active_document() != this)
        return;

    // NOTE: If our parent document needs a relayout, we must do that *first*.
    //       This is necessary as the parent layout may cause our viewport to change.
    if (navigable->container() && &navigable->container()->document() != this)
        navigable->container()->document().update_layout(reason);

    update_style();

    if (!m_needs_layout_update && m_layout_root)
        return;

    // NOTE: If this is a document hosting <template> contents, layout is unnecessary.
    if (m_created_for_appropriate_template_contents)
        return;

    invalidate_display_list();

    auto* document_element = this->document_element();
    auto viewport_rect = navigable->viewport_rect();

    auto timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);

    if (!m_layout_root || needs_layout_tree_update() || child_needs_layout_tree_update() || needs_full_layout_tree_update()) {
        Layout::TreeBuilder tree_builder;
        m_layout_root = as<Layout::Viewport>(*tree_builder.build(*this));

        if (document_element && document_element->layout_node()) {
            propagate_overflow_to_viewport(*document_element, *m_layout_root);
            propagate_scrollbar_width_to_viewport(*document_element, *m_layout_root);
        }

        set_needs_full_layout_tree_update(false);

        if constexpr (UPDATE_LAYOUT_DEBUG) {
            dbgln("TREEBUILD {} µs", timer.elapsed_time().to_microseconds());
        }
    }

    m_layout_root->for_each_in_inclusive_subtree_of_type<Layout::Box>([&](auto& child) {
        if (auto dom_node = child.dom_node(); dom_node && dom_node->is_element()) {
            child.set_has_size_containment(as<Element>(*dom_node).has_size_containment());
        }
        bool needs_layout_update = child.dom_node() && child.dom_node()->needs_layout_update();
        if (needs_layout_update || child.is_anonymous()) {
            child.reset_cached_intrinsic_sizes();
        }
        child.clear_contained_abspos_children();
        return TraversalDecision::Continue;
    });

    // Assign each box that establishes a formatting context a list of absolutely positioned children it should take care of during layout
    m_layout_root->for_each_in_inclusive_subtree_of_type<Layout::Box>([&](auto& child) {
        if (!child.is_absolutely_positioned())
            return TraversalDecision::Continue;
        if (auto* containing_block = child.containing_block()) {
            auto* closest_box_that_establishes_formatting_context = containing_block;
            while (closest_box_that_establishes_formatting_context) {
                if (closest_box_that_establishes_formatting_context == m_layout_root)
                    break;
                if (Layout::FormattingContext::formatting_context_type_created_by_box(*closest_box_that_establishes_formatting_context).has_value()) {
                    break;
                }
                closest_box_that_establishes_formatting_context = closest_box_that_establishes_formatting_context->containing_block();
            }
            VERIFY(closest_box_that_establishes_formatting_context);
            closest_box_that_establishes_formatting_context->add_contained_abspos_child(child);
        }
        return TraversalDecision::Continue;
    });

    Layout::LayoutState layout_state;

    {
        Layout::BlockFormattingContext root_formatting_context(layout_state, Layout::LayoutMode::Normal, *m_layout_root, nullptr);

        auto& viewport = static_cast<Layout::Viewport&>(*m_layout_root);
        auto& viewport_state = layout_state.get_mutable(viewport);
        viewport_state.set_content_width(viewport_rect.width());
        viewport_state.set_content_height(viewport_rect.height());

        if (document_element && document_element->layout_node()) {
            auto& icb_state = layout_state.get_mutable(as<Layout::NodeWithStyleAndBoxModelMetrics>(*document_element->layout_node()));
            icb_state.set_content_width(viewport_rect.width());
        }

        root_formatting_context.run(
            Layout::AvailableSpace(
                Layout::AvailableSize::make_definite(viewport_rect.width()),
                Layout::AvailableSize::make_definite(viewport_rect.height())));
    }

    layout_state.commit(*m_layout_root);

    // Broadcast the current viewport rect to any new paintables, so they know whether they're visible or not.
    inform_all_viewport_clients_about_the_current_viewport_rect();

    m_document->set_needs_display();
    set_needs_to_resolve_paint_only_properties();

    paintable()->assign_scroll_frames();

    // assign_clip_frames() needs border-radius be resolved
    update_paint_and_hit_testing_properties_if_needed();
    paintable()->assign_clip_frames();

    if (navigable->is_traversable()) {
        page().client().page_did_layout();
    }

    if (auto range = get_selection()->range()) {
        paintable()->recompute_selection_states(*range);
    }

    for_each_shadow_including_inclusive_descendant([](auto& node) {
        node.reset_needs_layout_update();
        return TraversalDecision::Continue;
    });

    // Scrolling by zero offset will clamp scroll offset back to valid range if it was out of bounds
    // after the viewport size change.
    if (auto window = this->window())
        window->scroll_by(0, 0);

    if constexpr (UPDATE_LAYOUT_DEBUG) {
        dbgln("LAYOUT {} {} µs", to_string(reason), timer.elapsed_time().to_microseconds());
    }
}

[[nodiscard]] static CSS::RequiredInvalidationAfterStyleChange update_style_recursively(Node& node, CSS::StyleComputer& style_computer, bool needs_inherited_style_update)
{
    bool const needs_full_style_update = node.document().needs_full_style_update();
    CSS::RequiredInvalidationAfterStyleChange invalidation;

    if (node.is_element())
        style_computer.push_ancestor(static_cast<Element const&>(node));

    // NOTE: If the current node has `display:none`, we can disregard all invalidation
    //       caused by its children, as they will not be rendered anyway.
    //       We will still recompute style for the children, though.
    bool is_display_none = false;

    CSS::RequiredInvalidationAfterStyleChange node_invalidation;
    if (is<Element>(node)) {
        if (needs_full_style_update || node.needs_style_update()) {
            node_invalidation = static_cast<Element&>(node).recompute_style();
        } else if (needs_inherited_style_update) {
            node_invalidation = static_cast<Element&>(node).recompute_inherited_style();
        }
        is_display_none = static_cast<Element&>(node).computed_properties()->display().is_none();
    }
    if (node_invalidation.relayout) {
        node.set_needs_layout_update(SetNeedsLayoutReason::StyleChange);
    }
    if (node_invalidation.rebuild_layout_tree) {
        // We mark layout tree for rebuild starting from parent element to correctly invalidate
        // "display" property change to/from "contents" value.
        if (auto* parent_element = node.parent_element()) {
            parent_element->set_needs_layout_tree_update(true);
        } else {
            node.set_needs_layout_tree_update(true);
        }
    }
    invalidation |= node_invalidation;
    node.set_needs_style_update(false);
    invalidation |= node_invalidation;

    bool children_need_inherited_style_update = !invalidation.is_none();
    if (needs_full_style_update || node.child_needs_style_update() || children_need_inherited_style_update) {
        if (node.is_element()) {
            if (auto shadow_root = static_cast<DOM::Element&>(node).shadow_root()) {
                if (needs_full_style_update || shadow_root->needs_style_update() || shadow_root->child_needs_style_update()) {
                    auto subtree_invalidation = update_style_recursively(*shadow_root, style_computer, children_need_inherited_style_update);
                    if (!is_display_none)
                        invalidation |= subtree_invalidation;
                }
            }
        }

        node.for_each_child([&](auto& child) {
            if (needs_full_style_update || child.needs_style_update() || children_need_inherited_style_update || child.child_needs_style_update()) {
                auto subtree_invalidation = update_style_recursively(child, style_computer, children_need_inherited_style_update);
                if (!is_display_none)
                    invalidation |= subtree_invalidation;
            }
            return IterationDecision::Continue;
        });
    }

    node.set_child_needs_style_update(false);

    if (node.is_element())
        style_computer.pop_ancestor(static_cast<Element const&>(node));

    return invalidation;
}

// This function makes a full pass over the entire DOM and converts "entire subtree needs style update"
// into "needs style update" for each inclusive descendant where it's found.
static void perform_pending_style_invalidations(Node& node, bool invalidate_entire_subtree)
{
    invalidate_entire_subtree |= node.entire_subtree_needs_style_update();

    if (invalidate_entire_subtree) {
        node.set_needs_style_update_internal(true);
        if (node.has_child_nodes())
            node.set_child_needs_style_update(true);
    }

    for (auto* child = node.first_child(); child; child = child->next_sibling()) {
        perform_pending_style_invalidations(*child, invalidate_entire_subtree);
    }

    if (node.is_element()) {
        auto& element = static_cast<Element&>(node);
        if (auto shadow_root = element.shadow_root()) {
            perform_pending_style_invalidations(*shadow_root, invalidate_entire_subtree);
            if (invalidate_entire_subtree)
                node.set_child_needs_style_update(true);
        }
    }

    node.set_entire_subtree_needs_style_update(false);
}

void Document::update_style()
{
    if (!browsing_context())
        return;

    update_animated_style_if_needed();

    // Associated with each top-level browsing context is a current transition generation that is incremented on each
    // style change event. [CSS-Transitions-2]
    m_transition_generation++;

    invalidate_style_of_elements_affected_by_has();

    if (!needs_full_style_update() && !needs_style_update() && !child_needs_style_update())
        return;

    perform_pending_style_invalidations(*this, false);

    // NOTE: If this is a document hosting <template> contents, style update is unnecessary.
    if (m_created_for_appropriate_template_contents)
        return;

    // Fetch the viewport rect once, instead of repeatedly, during style computation.
    style_computer().set_viewport_rect({}, viewport_rect());

    evaluate_media_rules();

    style_computer().reset_ancestor_filter();

    auto invalidation = update_style_recursively(*this, style_computer(), false);
    if (!invalidation.is_none())
        invalidate_display_list();
    if (invalidation.rebuild_stacking_context_tree)
        invalidate_stacking_context_tree();
    m_needs_full_style_update = false;
}

void Document::update_animated_style_if_needed()
{
    if (!m_needs_animated_style_update)
        return;

    for (auto& timeline : m_associated_animation_timelines) {
        for (auto& animation : timeline->associated_animations()) {
            if (animation->is_idle() || animation->is_finished())
                continue;
            if (auto effect = animation->effect()) {
                if (auto* target = effect->target())
                    target->reset_animated_css_properties();
                effect->update_computed_properties();
            }
        }
    }
    m_needs_animated_style_update = false;
}

void Document::update_paint_and_hit_testing_properties_if_needed()
{
    if (auto* paintable = this->paintable()) {
        paintable->refresh_scroll_state();
    }

    if (!m_needs_to_resolve_paint_only_properties)
        return;
    m_needs_to_resolve_paint_only_properties = false;
    if (auto* paintable = this->paintable()) {
        paintable->resolve_paint_only_properties();
    }
}

void Document::set_normal_link_color(Color color)
{
    m_normal_link_color = color;
}

void Document::set_active_link_color(Color color)
{
    m_active_link_color = color;
}

void Document::set_visited_link_color(Color color)
{
    m_visited_link_color = color;
}

Optional<Vector<String> const&> Document::supported_color_schemes() const
{
    return m_supported_color_schemes;
}

// https://html.spec.whatwg.org/multipage/semantics.html#meta-color-scheme
void Document::obtain_supported_color_schemes()
{
    m_supported_color_schemes = {};

    // 1. Let candidate elements be the list of all meta elements that meet the following criteria, in tree order:
    for_each_in_subtree_of_type<HTML::HTMLMetaElement>([&](HTML::HTMLMetaElement& element) {
        //     * the element is in a document tree;
        //     * the element has a name attribute, whose value is an ASCII case-insensitive match for color-scheme; and
        //     * the element has a content attribute.

        // 2. For each element in candidate elements:
        auto content = element.attribute(HTML::AttributeNames::content);
        if (element.name().has_value() && element.name()->equals_ignoring_ascii_case("color-scheme"sv) && content.has_value()) {
            // 1. Let parsed be the result of parsing a list of component values given the value of element's content attribute.
            auto context = CSS::Parser::ParsingParams { document() };
            auto parsed = parse_css_value(context, content.value(), CSS::PropertyID::ColorScheme);

            // 2. If parsed is a valid CSS 'color-scheme' property value, then return parsed.
            if (!parsed.is_null() && parsed->is_color_scheme()) {
                m_supported_color_schemes = parsed->as_color_scheme().schemes();
                return TraversalDecision::Break;
            }
        }

        return TraversalDecision::Continue;
    });

    // 3. Return null.
}

// https://html.spec.whatwg.org/multipage/semantics.html#meta-theme-color
void Document::obtain_theme_color()
{
    Color theme_color = Color::Transparent;

    // 1. Let candidate elements be the list of all meta elements that meet the following criteria, in tree order:
    for_each_in_subtree_of_type<HTML::HTMLMetaElement>([&](HTML::HTMLMetaElement& element) {
        //     * the element is in a document tree;
        //     * the element has a name attribute, whose value is an ASCII case-insensitive match for theme-color; and
        //     * the element has a content attribute.

        // 2. For each element in candidate elements:
        auto content = element.attribute(HTML::AttributeNames::content);
        if (element.name().has_value() && element.name()->equals_ignoring_ascii_case("theme-color"sv) && content.has_value()) {
            // 1. If element has a media attribute and the value of element's media attribute does not match the environment, then continue.
            auto context = CSS::Parser::ParsingParams { document() };
            auto media = element.attribute(HTML::AttributeNames::media);
            if (media.has_value()) {
                auto query = parse_media_query(context, media.value());
                if (query.is_null() || !window() || !query->evaluate(*window()))
                    return TraversalDecision::Continue;
            }

            // 2. Let value be the result of stripping leading and trailing ASCII whitespace from the value of element's content attribute.
            auto value = content->bytes_as_string_view().trim(Infra::ASCII_WHITESPACE);

            // 3. Let color be the result of parsing value.
            auto css_value = parse_css_value(context, value, CSS::PropertyID::Color);

            // 4. If color is not failure, then return color.
            if (!css_value.is_null() && css_value->has_color()) {
                Optional<Layout::NodeWithStyle const&> root_node;
                if (html_element() && html_element()->layout_node())
                    root_node = *html_element()->layout_node();

                theme_color = css_value->to_color(root_node);
                return TraversalDecision::Break;
            }
        }

        return TraversalDecision::Continue;
    });

    // 3. Return nothing(the page has no theme color).
    document().page().client().page_did_change_theme_color(theme_color);
}

Layout::Viewport const* Document::layout_node() const
{
    return static_cast<Layout::Viewport const*>(Node::layout_node());
}

Layout::Viewport* Document::layout_node()
{
    return static_cast<Layout::Viewport*>(Node::layout_node());
}

void Document::set_inspected_node(GC::Ptr<Node> node)
{
    m_inspected_node = node;
}

void Document::set_highlighted_node(GC::Ptr<Node> node, Optional<CSS::PseudoElement> pseudo_element)
{
    if (m_highlighted_node == node && m_highlighted_pseudo_element == pseudo_element)
        return;

    if (auto layout_node = highlighted_layout_node(); layout_node && layout_node->first_paintable())
        layout_node->first_paintable()->set_needs_display();

    m_highlighted_node = node;
    m_highlighted_pseudo_element = pseudo_element;

    if (auto layout_node = highlighted_layout_node(); layout_node && layout_node->first_paintable())
        layout_node->first_paintable()->set_needs_display();
}

GC::Ptr<Layout::Node> Document::highlighted_layout_node()
{
    if (!m_highlighted_node)
        return nullptr;

    if (!m_highlighted_pseudo_element.has_value() || !m_highlighted_node->is_element())
        return m_highlighted_node->layout_node();

    auto const& element = static_cast<Element const&>(*m_highlighted_node);
    return element.get_pseudo_element_node(m_highlighted_pseudo_element.value());
}

static Node* find_common_ancestor(Node* a, Node* b)
{
    if (!a || !b)
        return nullptr;

    if (a == b)
        return a;

    HashTable<Node*> ancestors;
    for (auto* node = a; node; node = node->parent_or_shadow_host())
        ancestors.set(node);

    for (auto* node = b; node; node = node->parent_or_shadow_host()) {
        if (ancestors.contains(node))
            return node;
    }

    return nullptr;
}

void Document::invalidate_style_of_elements_affected_by_has()
{
    if (m_pending_nodes_for_style_invalidation_due_to_presence_of_has.is_empty()) {
        return;
    }

    ScopeGuard clear_pending_nodes_guard = [&] {
        m_pending_nodes_for_style_invalidation_due_to_presence_of_has.clear();
    };

    // It's ok to call have_has_selectors() instead of may_have_has_selectors() here and force
    // rule cache build, because it's going to be build soon anyway, since we could get here
    // only from update_style().
    if (!style_computer().have_has_selectors()) {
        return;
    }

    for (auto const& node : m_pending_nodes_for_style_invalidation_due_to_presence_of_has) {
        if (node.is_null())
            continue;
        for (auto* ancestor = node.ptr(); ancestor; ancestor = ancestor->parent_or_shadow_host()) {
            if (!ancestor->is_element())
                continue;
            auto& element = static_cast<Element&>(*ancestor);
            element.invalidate_style_if_affected_by_has();

            auto* parent = ancestor->parent_or_shadow_host();
            if (!parent)
                return;

            // If any ancestor's sibling was tested against selectors like ".a:has(+ .b)" or ".a:has(~ .b)"
            // its style might be affected by the change in descendant node.
            parent->for_each_child_of_type<Element>([&](auto& ancestor_sibling_element) {
                if (ancestor_sibling_element.affected_by_has_pseudo_class_with_relative_selector_that_has_sibling_combinator())
                    ancestor_sibling_element.invalidate_style_if_affected_by_has();
                return IterationDecision::Continue;
            });
        }
    }
}

void Document::invalidate_style_for_elements_affected_by_hover_change(Node& old_new_hovered_common_ancestor, GC::Ptr<Node> hovered_node)
{
    auto const& hover_rules = style_computer().get_hover_rules();

    auto& root = old_new_hovered_common_ancestor.root();
    auto shadow_root = is<ShadowRoot>(root) ? static_cast<ShadowRoot const*>(&root) : nullptr;

    auto& style_computer = this->style_computer();
    auto does_rule_match_on_element = [&](Element const& element, CSS::MatchingRule const& rule) {
        auto rule_root = rule.shadow_root;
        auto from_user_agent_or_user_stylesheet = rule.cascade_origin == CSS::CascadeOrigin::UserAgent || rule.cascade_origin == CSS::CascadeOrigin::User;
        bool rule_is_relevant_for_current_scope = rule_root == shadow_root
            || (element.is_shadow_host() && rule_root == element.shadow_root())
            || from_user_agent_or_user_stylesheet;
        if (!rule_is_relevant_for_current_scope)
            return false;

        auto const& selector = rule.selector;
        if (selector.can_use_ancestor_filter() && style_computer.should_reject_with_ancestor_filter(selector))
            return false;

        SelectorEngine::MatchContext context;
        if (SelectorEngine::matches(selector, element, {}, context, {}))
            return true;
        if (element.has_pseudo_element(CSS::PseudoElement::Before)) {
            if (SelectorEngine::matches(selector, element, {}, context, CSS::PseudoElement::Before))
                return true;
        }
        if (element.has_pseudo_element(CSS::PseudoElement::After)) {
            if (SelectorEngine::matches(selector, element, {}, context, CSS::PseudoElement::After))
                return true;
        }
        return false;
    };

    auto matches_different_set_of_hover_rules_after_hovered_element_change = [&](Element const& element) {
        bool result = false;
        hover_rules.for_each_matching_rules(element, {}, [&](auto& rules) {
            for (auto& rule : rules) {
                bool before = does_rule_match_on_element(element, rule);
                TemporaryChange change { m_hovered_node, hovered_node };
                bool after = does_rule_match_on_element(element, rule);
                if (before != after) {
                    result = true;
                    return IterationDecision::Break;
                }
            }
            return IterationDecision::Continue;
        });
        return result;
    };

    Function<void(Node&)> invalidate_hovered_elements_recursively = [&](Node& node) -> void {
        if (node.is_element()) {
            auto& element = static_cast<Element&>(node);
            style_computer.push_ancestor(element);
            if (element.affected_by_hover() && matches_different_set_of_hover_rules_after_hovered_element_change(element)) {
                element.set_needs_style_update(true);
            }
        }

        node.for_each_child([&](auto& child) {
            invalidate_hovered_elements_recursively(child);
            return IterationDecision::Continue;
        });

        if (node.is_element())
            style_computer.pop_ancestor(static_cast<Element&>(node));
    };

    invalidate_hovered_elements_recursively(root);
}

void Document::set_hovered_node(Node* node)
{
    if (m_hovered_node.ptr() == node)
        return;

    GC::Ptr<Node> old_hovered_node = move(m_hovered_node);
    auto* common_ancestor = find_common_ancestor(old_hovered_node, node);

    GC::Ptr<Node> old_hovered_node_root = nullptr;
    GC::Ptr<Node> new_hovered_node_root = nullptr;
    if (old_hovered_node)
        old_hovered_node_root = old_hovered_node->root();
    if (node)
        new_hovered_node_root = node->root();
    if (old_hovered_node_root != new_hovered_node_root) {
        if (old_hovered_node_root)
            invalidate_style_for_elements_affected_by_hover_change(*old_hovered_node_root, node);
        if (new_hovered_node_root)
            invalidate_style_for_elements_affected_by_hover_change(*new_hovered_node_root, node);
    } else {
        invalidate_style_for_elements_affected_by_hover_change(*common_ancestor, node);
    }

    m_hovered_node = node;

    // https://w3c.github.io/uievents/#mouseout
    if (old_hovered_node && old_hovered_node != m_hovered_node) {
        UIEvents::MouseEventInit mouse_event_init {};
        mouse_event_init.bubbles = true;
        mouse_event_init.cancelable = true;
        mouse_event_init.composed = true;
        mouse_event_init.related_target = m_hovered_node;
        auto event = UIEvents::MouseEvent::create(realm(), UIEvents::EventNames::mouseout, mouse_event_init);
        old_hovered_node->dispatch_event(event);
    }

    // https://w3c.github.io/uievents/#mouseleave
    if (old_hovered_node && (!m_hovered_node || !m_hovered_node->is_descendant_of(*old_hovered_node))) {
        // FIXME: Check if we need to dispatch these events in a specific order.
        for (auto target = old_hovered_node; target && target.ptr() != common_ancestor; target = target->parent()) {
            // FIXME: Populate the event with mouse coordinates, etc.
            UIEvents::MouseEventInit mouse_event_init {};
            mouse_event_init.related_target = m_hovered_node;
            target->dispatch_event(UIEvents::MouseEvent::create(realm(), UIEvents::EventNames::mouseleave, mouse_event_init));
        }
    }

    // https://w3c.github.io/uievents/#mouseover
    if (m_hovered_node && m_hovered_node != old_hovered_node) {
        UIEvents::MouseEventInit mouse_event_init {};
        mouse_event_init.bubbles = true;
        mouse_event_init.cancelable = true;
        mouse_event_init.composed = true;
        mouse_event_init.related_target = old_hovered_node;
        auto event = UIEvents::MouseEvent::create(realm(), UIEvents::EventNames::mouseover, mouse_event_init);
        m_hovered_node->dispatch_event(event);
    }

    // https://w3c.github.io/uievents/#mouseenter
    if (m_hovered_node && (!old_hovered_node || !m_hovered_node->is_ancestor_of(*old_hovered_node))) {
        // FIXME: Check if we need to dispatch these events in a specific order.
        for (auto target = m_hovered_node; target && target.ptr() != common_ancestor; target = target->parent()) {
            // FIXME: Populate the event with mouse coordinates, etc.
            UIEvents::MouseEventInit mouse_event_init {};
            mouse_event_init.related_target = old_hovered_node;
            target->dispatch_event(UIEvents::MouseEvent::create(realm(), UIEvents::EventNames::mouseenter, mouse_event_init));
        }
    }
}

// https://html.spec.whatwg.org/multipage/dom.html#dom-document-getelementsbyname
GC::Ref<NodeList> Document::get_elements_by_name(FlyString const& name)
{
    return LiveNodeList::create(realm(), *this, LiveNodeList::Scope::Descendants, [name](auto const& node) {
        if (!is<HTML::HTMLElement>(node))
            return false;
        return as<HTML::HTMLElement>(node).name() == name;
    });
}

// https://html.spec.whatwg.org/multipage/obsolete.html#dom-document-applets
GC::Ref<HTMLCollection> Document::applets()
{
    if (!m_applets)
        m_applets = HTMLCollection::create(*this, HTMLCollection::Scope::Descendants, [](auto&) { return false; });
    return *m_applets;
}

// https://html.spec.whatwg.org/multipage/obsolete.html#dom-document-anchors
GC::Ref<HTMLCollection> Document::anchors()
{
    if (!m_anchors) {
        m_anchors = HTMLCollection::create(*this, HTMLCollection::Scope::Descendants, [](Element const& element) {
            return is<HTML::HTMLAnchorElement>(element) && element.name().has_value();
        });
    }
    return *m_anchors;
}

// https://html.spec.whatwg.org/multipage/dom.html#dom-document-images
GC::Ref<HTMLCollection> Document::images()
{
    if (!m_images) {
        m_images = HTMLCollection::create(*this, HTMLCollection::Scope::Descendants, [](Element const& element) {
            return is<HTML::HTMLImageElement>(element);
        });
    }
    return *m_images;
}

// https://html.spec.whatwg.org/multipage/dom.html#dom-document-embeds
GC::Ref<HTMLCollection> Document::embeds()
{
    if (!m_embeds) {
        m_embeds = HTMLCollection::create(*this, HTMLCollection::Scope::Descendants, [](Element const& element) {
            return is<HTML::HTMLEmbedElement>(element);
        });
    }
    return *m_embeds;
}

// https://html.spec.whatwg.org/multipage/dom.html#dom-document-plugins
GC::Ref<HTMLCollection> Document::plugins()
{
    return embeds();
}

// https://html.spec.whatwg.org/multipage/dom.html#dom-document-links
GC::Ref<HTMLCollection> Document::links()
{
    if (!m_links) {
        m_links = HTMLCollection::create(*this, HTMLCollection::Scope::Descendants, [](Element const& element) {
            return (is<HTML::HTMLAnchorElement>(element) || is<HTML::HTMLAreaElement>(element)) && element.has_attribute(HTML::AttributeNames::href);
        });
    }
    return *m_links;
}

// https://html.spec.whatwg.org/multipage/dom.html#dom-document-forms
GC::Ref<HTMLCollection> Document::forms()
{
    if (!m_forms) {
        m_forms = HTMLCollection::create(*this, HTMLCollection::Scope::Descendants, [](Element const& element) {
            return is<HTML::HTMLFormElement>(element);
        });
    }
    return *m_forms;
}

// https://html.spec.whatwg.org/multipage/dom.html#dom-document-scripts
GC::Ref<HTMLCollection> Document::scripts()
{
    if (!m_scripts) {
        m_scripts = HTMLCollection::create(*this, HTMLCollection::Scope::Descendants, [](Element const& element) {
            return is<HTML::HTMLScriptElement>(element);
        });
    }
    return *m_scripts;
}

// https://html.spec.whatwg.org/multipage/dom.html#dom-document-all
GC::Ref<HTML::HTMLAllCollection> Document::all()
{
    if (!m_all) {
        // The all attribute must return an HTMLAllCollection rooted at the Document node, whose filter matches all elements.
        m_all = HTML::HTMLAllCollection::create(*this, HTML::HTMLAllCollection::Scope::Descendants, [](Element const&) {
            return true;
        });
    }
    return *m_all;
}

// https://drafts.csswg.org/css-font-loading/#font-source
GC::Ref<CSS::FontFaceSet> Document::fonts()
{
    if (!m_fonts)
        m_fonts = CSS::FontFaceSet::create(realm());
    return *m_fonts;
}

// https://html.spec.whatwg.org/multipage/obsolete.html#dom-document-clear
void Document::clear()
{
    // Do nothing
}

// https://html.spec.whatwg.org/multipage/obsolete.html#dom-document-captureevents
void Document::capture_events()
{
    // Do nothing
}

// https://html.spec.whatwg.org/multipage/obsolete.html#dom-document-releaseevents
void Document::release_events()
{
    // Do nothing
}

Optional<Color> Document::normal_link_color() const
{
    return m_normal_link_color;
}

Optional<Color> Document::active_link_color() const
{
    return m_active_link_color;
}

Optional<Color> Document::visited_link_color() const
{
    return m_visited_link_color;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#relevant-settings-object
HTML::EnvironmentSettingsObject& Document::relevant_settings_object() const
{
    // Then, the relevant settings object for a platform object o is the environment settings object of the relevant Realm for o.
    return Bindings::principal_host_defined_environment_settings_object(realm());
}

// https://dom.spec.whatwg.org/#dom-document-createelement
WebIDL::ExceptionOr<GC::Ref<Element>> Document::create_element(String const& a_local_name, Variant<String, ElementCreationOptions> const& options)
{
    auto local_name = a_local_name.to_byte_string();

    // 1. If localName does not match the Name production, then throw an "InvalidCharacterError" DOMException.
    if (!is_valid_name(a_local_name))
        return WebIDL::InvalidCharacterError::create(realm(), "Invalid character in tag name."_string);

    // 2. If this is an HTML document, then set localName to localName in ASCII lowercase.
    if (document_type() == Type::HTML)
        local_name = local_name.to_lowercase();

    // 3. Let is be null.
    Optional<String> is_value;

    // 4. If options is a dictionary and options["is"] exists, then set is to it.
    if (options.has<ElementCreationOptions>()) {
        auto const& element_creation_options = options.get<ElementCreationOptions>();
        if (element_creation_options.is.has_value())
            is_value = element_creation_options.is.value();
    }

    // 5. Let namespace be the HTML namespace, if this is an HTML document or this’s content type is "application/xhtml+xml"; otherwise null.
    Optional<FlyString> namespace_;
    if (document_type() == Type::HTML || content_type() == "application/xhtml+xml"sv)
        namespace_ = Namespace::HTML;

    // 6. Return the result of creating an element given this, localName, namespace, null, is, and with the synchronous custom elements flag set.
    return TRY(DOM::create_element(*this, MUST(FlyString::from_deprecated_fly_string(local_name)), move(namespace_), {}, move(is_value), true));
}

// https://dom.spec.whatwg.org/#dom-document-createelementns
// https://dom.spec.whatwg.org/#internal-createelementns-steps
WebIDL::ExceptionOr<GC::Ref<Element>> Document::create_element_ns(Optional<FlyString> const& namespace_, String const& qualified_name, Variant<String, ElementCreationOptions> const& options)
{
    // 1. Let namespace, prefix, and localName be the result of passing namespace and qualifiedName to validate and extract.
    auto extracted_qualified_name = TRY(validate_and_extract(realm(), namespace_, qualified_name));

    // 2. Let is be null.
    Optional<String> is_value;

    // 3. If options is a dictionary and options["is"] exists, then set is to it.
    if (options.has<ElementCreationOptions>()) {
        auto const& element_creation_options = options.get<ElementCreationOptions>();
        if (element_creation_options.is.has_value())
            is_value = element_creation_options.is.value();
    }

    // 4. Return the result of creating an element given document, localName, namespace, prefix, is, and with the synchronous custom elements flag set.
    return TRY(DOM::create_element(*this, extracted_qualified_name.local_name(), extracted_qualified_name.namespace_(), extracted_qualified_name.prefix(), move(is_value), true));
}

GC::Ref<DocumentFragment> Document::create_document_fragment()
{
    return realm().create<DocumentFragment>(*this);
}

GC::Ref<Text> Document::create_text_node(String const& data)
{
    return realm().create<Text>(*this, data);
}

// https://dom.spec.whatwg.org/#dom-document-createcdatasection
WebIDL::ExceptionOr<GC::Ref<CDATASection>> Document::create_cdata_section(String const& data)
{
    // 1. If this is an HTML document, then throw a "NotSupportedError" DOMException.
    if (is_html_document())
        return WebIDL::NotSupportedError::create(realm(), "This operation is not supported for HTML documents"_string);

    // 2. If data contains the string "]]>", then throw an "InvalidCharacterError" DOMException.
    if (data.contains("]]>"sv))
        return WebIDL::InvalidCharacterError::create(realm(), "String may not contain ']]>'"_string);

    // 3. Return a new CDATASection node with its data set to data and node document set to this.
    return realm().create<CDATASection>(*this, data);
}

GC::Ref<Comment> Document::create_comment(String const& data)
{
    return realm().create<Comment>(*this, data);
}

// https://dom.spec.whatwg.org/#dom-document-createprocessinginstruction
WebIDL::ExceptionOr<GC::Ref<ProcessingInstruction>> Document::create_processing_instruction(String const& target, String const& data)
{
    // 1. If target does not match the Name production, then throw an "InvalidCharacterError" DOMException.
    if (!is_valid_name(target))
        return WebIDL::InvalidCharacterError::create(realm(), "Invalid character in target name."_string);

    // 2. If data contains the string "?>", then throw an "InvalidCharacterError" DOMException.
    if (data.contains("?>"sv))
        return WebIDL::InvalidCharacterError::create(realm(), "String may not contain '?>'"_string);

    // 3. Return a new ProcessingInstruction node, with target set to target, data set to data, and node document set to this.
    return realm().create<ProcessingInstruction>(*this, data, target);
}

GC::Ref<Range> Document::create_range()
{
    return Range::create(*this);
}

// https://dom.spec.whatwg.org/#dom-document-createevent
WebIDL::ExceptionOr<GC::Ref<Event>> Document::create_event(StringView interface)
{
    auto& realm = this->realm();

    // NOTE: This is named event here, since we do step 5 and 6 as soon as possible for each case.
    // 1. Let constructor be null.
    GC::Ptr<Event> event;

    // 2. If interface is an ASCII case-insensitive match for any of the strings in the first column in the following table,
    //      then set constructor to the interface in the second column on the same row as the matching string:
    if (Infra::is_ascii_case_insensitive_match(interface, "beforeunloadevent"sv)) {
        event = HTML::BeforeUnloadEvent::create(realm, FlyString {});
    } else if (Infra::is_ascii_case_insensitive_match(interface, "compositionevent"sv)) {
        event = UIEvents::CompositionEvent::create(realm, String {});
    } else if (Infra::is_ascii_case_insensitive_match(interface, "customevent"sv)) {
        event = CustomEvent::create(realm, FlyString {});
    } else if (Infra::is_ascii_case_insensitive_match(interface, "devicemotionevent"sv)) {
        event = Event::create(realm, FlyString {}); // FIXME: Create DeviceMotionEvent
    } else if (Infra::is_ascii_case_insensitive_match(interface, "deviceorientationevent"sv)) {
        event = Event::create(realm, FlyString {}); // FIXME: Create DeviceOrientationEvent
    } else if (Infra::is_ascii_case_insensitive_match(interface, "dragevent"sv)) {
        event = Event::create(realm, FlyString {}); // FIXME: Create DragEvent
    } else if (Infra::is_ascii_case_insensitive_match(interface, "event"sv)
        || Infra::is_ascii_case_insensitive_match(interface, "events"sv)) {
        event = Event::create(realm, FlyString {});
    } else if (Infra::is_ascii_case_insensitive_match(interface, "focusevent"sv)) {
        event = UIEvents::FocusEvent::create(realm, FlyString {});
    } else if (Infra::is_ascii_case_insensitive_match(interface, "hashchangeevent"sv)) {
        event = HTML::HashChangeEvent::create(realm, FlyString {}, {});
    } else if (Infra::is_ascii_case_insensitive_match(interface, "htmlevents"sv)) {
        event = Event::create(realm, FlyString {});
    } else if (Infra::is_ascii_case_insensitive_match(interface, "keyboardevent"sv)) {
        event = UIEvents::KeyboardEvent::create(realm, String {});
    } else if (Infra::is_ascii_case_insensitive_match(interface, "messageevent"sv)) {
        event = HTML::MessageEvent::create(realm, String {});
    } else if (Infra::is_ascii_case_insensitive_match(interface, "mouseevent"sv)
        || Infra::is_ascii_case_insensitive_match(interface, "mouseevents"sv)) {
        event = UIEvents::MouseEvent::create(realm, FlyString {});
    } else if (Infra::is_ascii_case_insensitive_match(interface, "storageevent"sv)) {
        event = Event::create(realm, FlyString {}); // FIXME: Create StorageEvent
    } else if (Infra::is_ascii_case_insensitive_match(interface, "svgevents"sv)) {
        event = Event::create(realm, FlyString {});
    } else if (Infra::is_ascii_case_insensitive_match(interface, "textevent"sv)) {
        event = UIEvents::TextEvent::create(realm, FlyString {});
    } else if (Infra::is_ascii_case_insensitive_match(interface, "touchevent"sv)) {
        event = Event::create(realm, FlyString {}); // FIXME: Create TouchEvent
    } else if (Infra::is_ascii_case_insensitive_match(interface, "uievent"sv)
        || Infra::is_ascii_case_insensitive_match(interface, "uievents"sv)) {
        event = UIEvents::UIEvent::create(realm, FlyString {});
    }

    // 3. If constructor is null, then throw a "NotSupportedError" DOMException.
    if (!event) {
        return WebIDL::NotSupportedError::create(realm, "No constructor for interface found"_string);
    }

    // FIXME: 4. If the interface indicated by constructor is not exposed on the relevant global object of this, then throw a "NotSupportedError" DOMException.

    // NOTE: These are done in the if-chain above
    // 5. Let event be the result of creating an event given constructor.
    // 6. Initialize event’s type attribute to the empty string.
    // 7. Initialize event’s timeStamp attribute to the result of calling current high resolution time with this’s relevant global object.
    // NOTE: This is handled by each constructor.

    // 8. Initialize event’s isTrusted attribute to false.
    event->set_is_trusted(false);

    // 9. Unset event’s initialized flag.
    event->set_initialized(false);

    // 10. Return event.
    return GC::Ref(*event);
}

void Document::set_pending_parsing_blocking_script(HTML::HTMLScriptElement* script)
{
    m_pending_parsing_blocking_script = script;
}

GC::Ref<HTML::HTMLScriptElement> Document::take_pending_parsing_blocking_script(Badge<HTML::HTMLParser>)
{
    VERIFY(m_pending_parsing_blocking_script);
    auto script = m_pending_parsing_blocking_script;
    m_pending_parsing_blocking_script = nullptr;
    return *script;
}

void Document::add_script_to_execute_when_parsing_has_finished(Badge<HTML::HTMLScriptElement>, HTML::HTMLScriptElement& script)
{
    m_scripts_to_execute_when_parsing_has_finished.append(script);
}

Vector<GC::Root<HTML::HTMLScriptElement>> Document::take_scripts_to_execute_when_parsing_has_finished(Badge<HTML::HTMLParser>)
{
    Vector<GC::Root<HTML::HTMLScriptElement>> handles;
    for (auto script : m_scripts_to_execute_when_parsing_has_finished)
        handles.append(GC::make_root(script));
    m_scripts_to_execute_when_parsing_has_finished.clear();
    return handles;
}

void Document::add_script_to_execute_as_soon_as_possible(Badge<HTML::HTMLScriptElement>, HTML::HTMLScriptElement& script)
{
    m_scripts_to_execute_as_soon_as_possible.append(script);
}

Vector<GC::Root<HTML::HTMLScriptElement>> Document::take_scripts_to_execute_as_soon_as_possible(Badge<HTML::HTMLParser>)
{
    Vector<GC::Root<HTML::HTMLScriptElement>> handles;
    for (auto script : m_scripts_to_execute_as_soon_as_possible)
        handles.append(GC::make_root(script));
    m_scripts_to_execute_as_soon_as_possible.clear();
    return handles;
}

void Document::add_script_to_execute_in_order_as_soon_as_possible(Badge<HTML::HTMLScriptElement>, HTML::HTMLScriptElement& script)
{
    m_scripts_to_execute_in_order_as_soon_as_possible.append(script);
}

Vector<GC::Root<HTML::HTMLScriptElement>> Document::take_scripts_to_execute_in_order_as_soon_as_possible(Badge<HTML::HTMLParser>)
{
    Vector<GC::Root<HTML::HTMLScriptElement>> handles;
    for (auto script : m_scripts_to_execute_in_order_as_soon_as_possible)
        handles.append(GC::make_root(script));
    m_scripts_to_execute_in_order_as_soon_as_possible.clear();
    return handles;
}

// https://dom.spec.whatwg.org/#dom-document-importnode
WebIDL::ExceptionOr<GC::Ref<Node>> Document::import_node(GC::Ref<Node> node, bool deep)
{
    // 1. If node is a document or shadow root, then throw a "NotSupportedError" DOMException.
    if (is<Document>(*node) || is<ShadowRoot>(*node))
        return WebIDL::NotSupportedError::create(realm(), "Cannot import a document or shadow root."_string);

    // 2. Return a clone of node, with this and the clone children flag set if deep is true.
    return node->clone_node(this, deep);
}

// https://dom.spec.whatwg.org/#concept-node-adopt
void Document::adopt_node(Node& node)
{
    // 1. Let oldDocument be node’s node document.
    auto& old_document = node.document();

    // 2. If node’s parent is non-null, then remove node.
    if (node.parent())
        node.remove();

    // 3. If document is not oldDocument, then:
    if (&old_document != this) {
        // 1. For each inclusiveDescendant in node’s shadow-including inclusive descendants:
        node.for_each_shadow_including_inclusive_descendant([&](DOM::Node& inclusive_descendant) {
            // 1. Set inclusiveDescendant’s node document to document.
            inclusive_descendant.set_document(Badge<Document> {}, *this);

            // FIXME: 2. If inclusiveDescendant is an element, then set the node document of each attribute in inclusiveDescendant’s
            //           attribute list to document.
            return TraversalDecision::Continue;
        });

        // 2. For each inclusiveDescendant in node’s shadow-including inclusive descendants that is custom,
        //    enqueue a custom element callback reaction with inclusiveDescendant, callback name "adoptedCallback",
        //    and an argument list containing oldDocument and document.
        node.for_each_shadow_including_inclusive_descendant([&](DOM::Node& inclusive_descendant) {
            if (!is<DOM::Element>(inclusive_descendant))
                return TraversalDecision::Continue;

            auto& element = static_cast<DOM::Element&>(inclusive_descendant);
            if (element.is_custom()) {
                auto& vm = this->vm();

                GC::RootVector<JS::Value> arguments { vm.heap() };
                arguments.append(&old_document);
                arguments.append(this);

                element.enqueue_a_custom_element_callback_reaction(HTML::CustomElementReactionNames::adoptedCallback, move(arguments));
            }

            return TraversalDecision::Continue;
        });

        // 3. For each inclusiveDescendant in node’s shadow-including inclusive descendants, in shadow-including tree order,
        //    run the adopting steps with inclusiveDescendant and oldDocument.
        node.for_each_shadow_including_inclusive_descendant([&](auto& inclusive_descendant) {
            inclusive_descendant.adopted_from(old_document);
            return TraversalDecision::Continue;
        });

        // Transfer NodeIterators rooted at `node` from old_document to this document.
        Vector<NodeIterator&> node_iterators_to_transfer;
        for (auto node_iterator : old_document.m_node_iterators) {
            if (node_iterator->root().ptr() == &node)
                node_iterators_to_transfer.append(*node_iterator);
        }

        for (auto& node_iterator : node_iterators_to_transfer) {
            old_document.m_node_iterators.remove(&node_iterator);
            m_node_iterators.set(&node_iterator);
        }
    }
}

// https://dom.spec.whatwg.org/#dom-document-adoptnode
WebIDL::ExceptionOr<GC::Ref<Node>> Document::adopt_node_binding(GC::Ref<Node> node)
{
    if (is<Document>(*node))
        return WebIDL::NotSupportedError::create(realm(), "Cannot adopt a document into a document"_string);

    if (is<ShadowRoot>(*node))
        return WebIDL::HierarchyRequestError::create(realm(), "Cannot adopt a shadow root into a document"_string);

    if (is<DocumentFragment>(*node) && as<DocumentFragment>(*node).host())
        return node;

    adopt_node(*node);

    return node;
}

DocumentType const* Document::doctype() const
{
    return first_child_of_type<DocumentType>();
}

String const& Document::compat_mode() const
{
    static String const back_compat = "BackCompat"_string;
    static String const css1_compat = "CSS1Compat"_string;

    if (m_quirks_mode == QuirksMode::Yes)
        return back_compat;

    return css1_compat;
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-documentorshadowroot-activeelement
void Document::update_active_element()
{
    // 1. Let candidate be the DOM anchor of the focused area of this DocumentOrShadowRoot's node document.
    Node* candidate = focused_element();

    // 2. Set candidate to the result of retargeting candidate against this DocumentOrShadowRoot.
    candidate = as<Node>(retarget(candidate, this));

    // 3. If candidate's root is not this DocumentOrShadowRoot, then return null.
    if (&candidate->root() != this) {
        set_active_element(nullptr);
        return;
    }

    // 4. If candidate is not a Document object, then return candidate.
    if (!is<Document>(candidate)) {
        set_active_element(as<Element>(candidate));
        return;
    }

    auto* candidate_document = static_cast<Document*>(candidate);

    // 5. If candidate has a body element, then return that body element.
    if (candidate_document->body()) {
        set_active_element(candidate_document->body());
        return;
    }

    // 6. If candidate's document element is non-null, then return that document element.
    if (candidate_document->document_element()) {
        set_active_element(candidate_document->document_element());
        return;
    }

    // 7. Return null.
    set_active_element(nullptr);
    return;
}

void Document::set_focused_element(Element* element)
{
    if (m_focused_element.ptr() == element)
        return;

    GC::Ptr<Element> old_focused_element = move(m_focused_element);

    if (old_focused_element)
        old_focused_element->did_lose_focus();

    m_focused_element = element;

    if (auto* invalidation_target = find_common_ancestor(old_focused_element, m_focused_element) ?: this)
        invalidation_target->invalidate_style(StyleInvalidationReason::FocusedElementChange);

    if (m_focused_element)
        m_focused_element->did_receive_focus();

    if (paintable())
        paintable()->set_needs_display();

    // Scroll the viewport if necessary to make the newly focused element visible.
    if (m_focused_element) {
        m_focused_element->queue_an_element_task(HTML::Task::Source::UserInteraction, [&]() {
            ScrollIntoViewOptions scroll_options;
            scroll_options.block = Bindings::ScrollLogicalPosition::Nearest;
            scroll_options.inline_ = Bindings::ScrollLogicalPosition::Nearest;
            (void)m_focused_element->scroll_into_view(scroll_options);
        });
    }

    update_active_element();
}

void Document::set_active_element(Element* element)
{
    if (m_active_element.ptr() == element)
        return;

    GC::Ptr<Node> old_active_element = move(m_active_element);
    m_active_element = element;

    if (auto* invalidation_target = find_common_ancestor(old_active_element, m_active_element) ?: this)
        invalidation_target->invalidate_style(StyleInvalidationReason::TargetElementChange);

    if (paintable())
        paintable()->set_needs_display();
}

void Document::set_target_element(Element* element)
{
    if (m_target_element.ptr() == element)
        return;

    GC::Ptr<Element> old_target_element = move(m_target_element);
    m_target_element = element;

    if (auto* invalidation_target = find_common_ancestor(old_target_element, m_target_element) ?: this)
        invalidation_target->invalidate_style(StyleInvalidationReason::TargetElementChange);

    if (paintable())
        paintable()->set_needs_display();
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#the-indicated-part-of-the-document
Document::IndicatedPart Document::determine_the_indicated_part() const
{
    // For an HTML document document, the following processing model must be followed to determine its indicated part:

    // 1. Let fragment be document's URL's fragment.
    auto fragment = url().fragment();

    // 2. If fragment is the empty string, then return the special value top of the document.
    if (!fragment.has_value() || fragment->is_empty())
        return Document::TopOfTheDocument {};

    // 3. Let potentialIndicatedElement be the result of finding a potential indicated element given document and fragment.
    auto* potential_indicated_element = find_a_potential_indicated_element(*fragment);

    // 4. If potentialIndicatedElement is not null, then return potentialIndicatedElement.
    if (potential_indicated_element)
        return potential_indicated_element;

    // 5. Let fragmentBytes be the result of percent-decoding fragment.
    // 6. Let decodedFragment be the result of running UTF-8 decode without BOM on fragmentBytes.
    auto decoded_fragment = String::from_utf8_with_replacement_character(URL::percent_decode(*fragment), String::WithBOMHandling::No);

    // 7. Set potentialIndicatedElement to the result of finding a potential indicated element given document and decodedFragment.
    potential_indicated_element = find_a_potential_indicated_element(decoded_fragment);

    // 8. If potentialIndicatedElement is not null, then return potentialIndicatedElement.
    if (potential_indicated_element)
        return potential_indicated_element;

    // 9. If decodedFragment is an ASCII case-insensitive match for the string top, then return the top of the document.
    if (Infra::is_ascii_case_insensitive_match(decoded_fragment, "top"sv))
        return Document::TopOfTheDocument {};

    // 10. Return null.
    return nullptr;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#find-a-potential-indicated-element
Element* Document::find_a_potential_indicated_element(FlyString const& fragment) const
{
    // To find a potential indicated element given a Document document and a string fragment, run these steps:

    // 1. If there is an element in the document tree whose root is document and that has an ID equal to
    //    fragment, then return the first such element in tree order.
    if (auto element = get_element_by_id(fragment))
        return const_cast<Element*>(element.ptr());

    // 2. If there is an a element in the document tree whose root is document that has a name attribute
    //    whose value is equal to fragment, then return the first such element in tree order.
    Element* element_with_name = nullptr;
    root().for_each_in_subtree_of_type<Element>([&](Element const& element) {
        if (element.name() == fragment) {
            element_with_name = const_cast<Element*>(&element);
            return TraversalDecision::Break;
        }
        return TraversalDecision::Continue;
    });
    if (element_with_name)
        return element_with_name;

    // 3. Return null.
    return nullptr;
}

// https://drafts.csswg.org/css-transitions-2/#event-dispatch
void Document::dispatch_events_for_transition(GC::Ref<CSS::CSSTransition> transition)
{
    auto previous_phase = transition->previous_phase();

    using Phase = CSS::CSSTransition::Phase;
    // The transition phase of a transition is initially ‘idle’ and is updated on each
    // animation frame according to the first matching condition from below:
    auto transition_phase = Phase::Idle;

    if (!transition->effect()) {
        // If the transition has no associated effect,
        if (!transition->current_time().has_value()) {
            // If the transition has an unresolved current time,
            //   The transition phase is ‘idle’.
        } else if (transition->current_time().value() < 0.0) {
            // If the transition has a current time < 0,
            //   The transition phase is ‘before’.
            transition_phase = Phase::Before;
        } else {
            // Otherwise,
            //   The transition phase is ‘after’.
            transition_phase = Phase::After;
        }
    } else if (transition->pending() && (previous_phase == Phase::Idle || previous_phase == Phase::Pending)) {
        // If the transition has a pending play task or a pending pause task
        // and its phase was previously ‘idle’ or ‘pending’,
        //   The transition phase is ‘pending’.
        transition_phase = Phase::Pending;
    } else {
        // Otherwise,
        //   The transition phase is the phase of its associated effect.
        transition_phase = Phase(to_underlying(transition->effect()->phase()));
    }

    enum class Interval : u8 {
        Start,
        End,
        ActiveTime,
    };

    auto dispatch_event = [&](FlyString const& type, Interval interval) {
        // The target for a transition event is the transition’s owning element. If there is no owning element,
        // no transition events are dispatched.
        if (!transition->effect() || !transition->owning_element())
            return;

        auto effect = transition->effect();

        double elapsed_time = [&]() {
            if (interval == Interval::Start)
                return max(min(-effect->start_delay(), effect->active_duration()), 0) / 1000;
            if (interval == Interval::End)
                return max(min(transition->associated_effect_end() - effect->start_delay(), effect->active_duration()), 0) / 1000;
            if (interval == Interval::ActiveTime) {
                // The active time of the animation at the moment it was canceled calculated using a fill mode of both.
                // FIXME: Compute this properly.
                return 0.0;
            }
            VERIFY_NOT_REACHED();
        }();

        append_pending_animation_event({
            .event = CSS::TransitionEvent::create(
                transition->owning_element()->realm(),
                type,
                CSS::TransitionEventInit {
                    { .bubbles = true },
                    // FIXME: Correctly set property_name and pseudo_element
                    String {},
                    elapsed_time,
                    String {},
                }),
            .animation = transition,
            .target = *transition->owning_element(),
            .scheduled_event_time = HighResolutionTime::unsafe_shared_current_time(),
        });
    };

    if (previous_phase == Phase::Idle) {
        if (transition_phase == Phase::Pending || transition_phase == Phase::Before)
            dispatch_event(HTML::EventNames::transitionrun, Interval::Start);

        if (transition_phase == Phase::Active) {
            dispatch_event(HTML::EventNames::transitionrun, Interval::Start);
            dispatch_event(HTML::EventNames::transitionstart, Interval::Start);
        }

        if (transition_phase == Phase::After) {
            dispatch_event(HTML::EventNames::transitionrun, Interval::Start);
            dispatch_event(HTML::EventNames::transitionstart, Interval::Start);
            dispatch_event(HTML::EventNames::transitionend, Interval::End);
        }
    } else if (previous_phase == Phase::Pending || previous_phase == Phase::Before) {
        if (transition_phase == Phase::Active)
            dispatch_event(HTML::EventNames::transitionstart, Interval::Start);

        if (transition_phase == Phase::After) {
            dispatch_event(HTML::EventNames::transitionstart, Interval::Start);
            dispatch_event(HTML::EventNames::transitionend, Interval::End);
        }
    } else if (previous_phase == Phase::Active) {
        if (transition_phase == Phase::After)
            dispatch_event(HTML::EventNames::transitionend, Interval::End);

        if (transition_phase == Phase::Before)
            dispatch_event(HTML::EventNames::transitionend, Interval::Start);
    } else if (previous_phase == Phase::After) {
        if (transition_phase == Phase::Active)
            dispatch_event(HTML::EventNames::transitionstart, Interval::End);

        if (transition_phase == Phase::Before) {
            dispatch_event(HTML::EventNames::transitionstart, Interval::End);
            dispatch_event(HTML::EventNames::transitionend, Interval::Start);
        }
    }

    if (transition_phase == Phase::Idle) {
        if (previous_phase != Phase::Idle && previous_phase != Phase::After)
            dispatch_event(HTML::EventNames::animationstart, Interval::ActiveTime);
    }

    transition->set_previous_phase(transition_phase);
}

// https://www.w3.org/TR/css-animations-2/#event-dispatch
void Document::dispatch_events_for_animation_if_necessary(GC::Ref<Animations::Animation> animation)
{
    if (animation->is_css_transition()) {
        dispatch_events_for_transition(as<CSS::CSSTransition>(*animation));
        return;
    }

    // Each time a new animation frame is established and the animation does not have a pending play task or pending
    // pause task, the events to dispatch are determined by comparing the animation’s phase before and after
    // establishing the new animation frame as follows:
    auto effect = animation->effect();
    if (!effect || !effect->is_keyframe_effect() || !animation->is_css_animation() || animation->pending())
        return;

    auto& css_animation = as<CSS::CSSAnimation>(*animation);

    GC::Ptr<Element> target = effect->target();
    if (!target)
        return;

    auto previous_phase = effect->previous_phase();
    auto current_phase = effect->phase();
    auto current_iteration = effect->current_iteration().value_or(0.0);

    auto owning_element = css_animation.owning_element();

    auto dispatch_event = [&](FlyString const& name, double elapsed_time_ms) {
        auto elapsed_time_seconds = elapsed_time_ms / 1000;

        append_pending_animation_event({
            .event = CSS::AnimationEvent::create(
                owning_element->realm(),
                name,
                {
                    { .bubbles = true },
                    css_animation.id(),
                    elapsed_time_seconds,
                }),
            .animation = css_animation,
            .target = *target,
            .scheduled_event_time = HighResolutionTime::unsafe_shared_current_time(),
        });
    };

    // For calculating the elapsedTime of each event, the following definitions are used:

    // - interval start = max(min(-start delay, active duration), 0)
    auto interval_start = max(min(-effect->start_delay(), effect->active_duration()), 0.0);

    // - interval end = max(min(associated effect end - start delay, active duration), 0)
    auto interval_end = max(min(effect->end_time() - effect->start_delay(), effect->active_duration()), 0.0);

    switch (previous_phase) {
    case Animations::AnimationEffect::Phase::Before:
        [[fallthrough]];
    case Animations::AnimationEffect::Phase::Idle:
        if (current_phase == Animations::AnimationEffect::Phase::Active) {
            dispatch_event(HTML::EventNames::animationstart, interval_start);
        } else if (current_phase == Animations::AnimationEffect::Phase::After) {
            dispatch_event(HTML::EventNames::animationstart, interval_start);
            dispatch_event(HTML::EventNames::animationend, interval_end);
        }
        break;
    case Animations::AnimationEffect::Phase::Active:
        if (current_phase == Animations::AnimationEffect::Phase::Before) {
            dispatch_event(HTML::EventNames::animationend, interval_start);
        } else if (current_phase == Animations::AnimationEffect::Phase::Active) {
            auto previous_current_iteration = effect->previous_current_iteration();
            if (previous_current_iteration != current_iteration) {
                // The elapsed time for an animationiteration event is defined as follows:

                // 1. Let previous current iteration be the current iteration from the previous animation frame.

                // 2. If previous current iteration is greater than current iteration, let iteration boundary be current iteration + 1,
                //    otherwise let it be current iteration.
                auto iteration_boundary = previous_current_iteration > current_iteration ? current_iteration + 1 : current_iteration;

                // 3. The elapsed time is the result of evaluating (iteration boundary - iteration start) × iteration duration).
                auto iteration_duration_variant = effect->iteration_duration();
                auto iteration_duration = iteration_duration_variant.has<String>() ? 0.0 : iteration_duration_variant.get<double>();
                auto elapsed_time = (iteration_boundary - effect->iteration_start()) * iteration_duration;

                dispatch_event(HTML::EventNames::animationiteration, elapsed_time);
            }
        } else if (current_phase == Animations::AnimationEffect::Phase::After) {
            dispatch_event(HTML::EventNames::animationend, interval_end);
        }
        break;
    case Animations::AnimationEffect::Phase::After:
        if (current_phase == Animations::AnimationEffect::Phase::Active) {
            dispatch_event(HTML::EventNames::animationstart, interval_end);
        } else if (current_phase == Animations::AnimationEffect::Phase::Before) {
            dispatch_event(HTML::EventNames::animationstart, interval_end);
            dispatch_event(HTML::EventNames::animationend, interval_start);
        }
        break;
    }

    if (current_phase == Animations::AnimationEffect::Phase::Idle && previous_phase != Animations::AnimationEffect::Phase::Idle && previous_phase != Animations::AnimationEffect::Phase::After) {
        // FIXME: Calculate a non-zero time when the animation is cancelled by means other than calling cancel()
        auto cancel_time = animation->release_saved_cancel_time().value_or(0.0);
        dispatch_event(HTML::EventNames::animationcancel, cancel_time);
    }

    effect->set_previous_phase(current_phase);
    effect->set_previous_current_iteration(current_iteration);
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#scroll-to-the-fragment-identifier
void Document::scroll_to_the_fragment()
{
    // To scroll to the fragment given a Document document:

    // 1. If document's indicated part is null, then set document's target element to null.
    auto indicated_part = determine_the_indicated_part();
    if (indicated_part.has<Element*>() && indicated_part.get<Element*>() == nullptr) {
        set_target_element(nullptr);
    }

    // 2. Otherwise, if document's indicated part is top of the document, then:
    else if (indicated_part.has<TopOfTheDocument>()) {
        // 1. Set document's target element to null.
        set_target_element(nullptr);

        // 2. Scroll to the beginning of the document for document. [CSSOMVIEW]
        scroll_to_the_beginning_of_the_document();

        // 3. Return.
        return;
    }

    // 3. Otherwise:
    else {
        // 1. Assert: document's indicated part is an element.
        VERIFY(indicated_part.has<Element*>());

        // 2. Let target be document's indicated part.
        auto target = indicated_part.get<Element*>();

        // 3. Set document's target element to target.
        set_target_element(target);

        // FIXME: 4. Run the ancestor details revealing algorithm on target.

        // FIXME: 5. Run the ancestor hidden-until-found revealing algorithm on target.

        // 6. Scroll target into view, with behavior set to "auto", block set to "start", and inline set to "nearest". [CSSOMVIEW]
        ScrollIntoViewOptions scroll_options;
        scroll_options.block = Bindings::ScrollLogicalPosition::Start;
        scroll_options.inline_ = Bindings::ScrollLogicalPosition::Nearest;
        (void)target->scroll_into_view(scroll_options);

        // 7. Run the focusing steps for target, with the Document's viewport as the fallback target.
        // FIXME: Pass the Document's viewport somehow.
        HTML::run_focusing_steps(target);

        // FIXME: 8. Move the sequential focus navigation starting point to target.
    }
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#try-to-scroll-to-the-fragment
void Document::try_to_scroll_to_the_fragment()
{
    // FIXME: According to the spec we should only scroll here if document has no parser or parsing has stopped.
    //        It should be ok to remove this after we implement navigation events and scrolling will happen in
    //        "process scroll behavior".
    //  To try to scroll to the fragment for a Document document, perform the following steps in parallel:
    //  1. Wait for an implementation-defined amount of time. (This is intended to allow the user agent to
    //     optimize the user experience in the face of performance concerns.)
    //  2. Queue a global task on the navigation and traversal task source given document's relevant global
    //     object to run these steps:
    //      1. If document has no parser, or its parser has stopped parsing, or the user agent has reason to
    //         believe the user is no longer interested in scrolling to the fragment, then abort these steps.
    //      2. Scroll to the fragment given document.
    //      3. If document's indicated part is still null, then try to scroll to the fragment for document.

    scroll_to_the_fragment();
}

// https://drafts.csswg.org/cssom-view-1/#scroll-to-the-beginning-of-the-document
void Document::scroll_to_the_beginning_of_the_document()
{
    // FIXME: Actually implement this algorithm
    if (auto navigable = this->navigable())
        navigable->perform_scroll_of_viewport({ 0, 0 });
}

StringView Document::ready_state() const
{
    switch (m_readiness) {
    case HTML::DocumentReadyState::Loading:
        return "loading"sv;
    case HTML::DocumentReadyState::Interactive:
        return "interactive"sv;
    case HTML::DocumentReadyState::Complete:
        return "complete"sv;
    }
    VERIFY_NOT_REACHED();
}

// https://html.spec.whatwg.org/multipage/dom.html#update-the-current-document-readiness
void Document::update_readiness(HTML::DocumentReadyState readiness_value)
{
    // 1. If document's current document readiness equals readinessValue, then return.
    if (m_readiness == readiness_value)
        return;

    // 2. Set document's current document readiness to readinessValue.
    m_readiness = readiness_value;

    // 3. If document is associated with an HTML parser, then:
    if (m_parser) {
        // 1. Let now be the current high resolution time given document's relevant global object.
        auto now = HighResolutionTime::current_high_resolution_time(relevant_global_object(*this));

        // 2. If readinessValue is "complete", and document's load timing info's DOM complete time is 0,
        //    then set document's load timing info's DOM complete time to now.
        if (readiness_value == HTML::DocumentReadyState::Complete && m_load_timing_info.dom_complete_time == 0) {
            m_load_timing_info.dom_complete_time = now;
        }
        // 3. Otherwise, if readinessValue is "interactive", and document's load timing info's DOM interactive time is 0,
        //    then set document's load timing info's DOM interactive time to now.
        else if (readiness_value == HTML::DocumentReadyState::Interactive && m_load_timing_info.dom_interactive_time == 0) {
            m_load_timing_info.dom_interactive_time = now;
        }
    }

    // 4. Fire an event named readystatechange at document.
    dispatch_event(Event::create(realm(), HTML::EventNames::readystatechange));

    if (readiness_value == HTML::DocumentReadyState::Complete) {
        auto navigable = this->navigable();
        if (navigable && navigable->is_traversable()) {
            if (!is_decoded_svg()) {
                HTML::HTMLLinkElement::load_fallback_favicon_if_needed(*this).release_value_but_fixme_should_propagate_errors();
            }
            navigable->traversable_navigable()->page().client().page_did_finish_loading(url());
        } else {
            m_needs_to_call_page_did_load = true;
        }
    }

    notify_each_document_observer([&](auto const& document_observer) {
        return document_observer.document_readiness_observer();
    },
        m_readiness);
}

// https://html.spec.whatwg.org/multipage/dom.html#dom-document-lastmodified
String Document::last_modified() const
{
    // The lastModified attribute, on getting, must return the date and time of the Document's source file's
    // last modification, in the user's local time zone, in the following format:

    // 1. The month component of the date.
    // 2. A U+002F SOLIDUS character (/).
    // 3. The day component of the date.
    // 4. A U+002F SOLIDUS character (/).
    // 5. The year component of the date.
    // 6. A U+0020 SPACE character.
    // 7. The hours component of the time.
    // 8. A U+003A COLON character (:).
    // 9. The minutes component of the time.
    // 10. A U+003A COLON character (:).
    // 11. The seconds component of the time.

    // The Document's source file's last modification date and time must be derived from relevant features
    // of the networking protocols used, e.g. from the value of the HTTP `Last-Modified` header of the document,
    // or from metadata in the file system for local files. If the last modification date and time are not known,
    // the attribute must return the current date and time in the above format.
    constexpr auto format_string = "%m/%d/%Y %H:%M:%S"sv;

    if (m_last_modified.has_value())
        return MUST(m_last_modified.value().to_string(format_string));

    return MUST(Core::DateTime::now().to_string(format_string));
}

Page& Document::page()
{
    return m_page;
}

Page const& Document::page() const
{
    return m_page;
}

EventTarget* Document::get_parent(Event const& event)
{
    if (event.type() == HTML::EventNames::load)
        return nullptr;

    return m_window;
}

// https://html.spec.whatwg.org/multipage/document-lifecycle.html#completely-loaded
bool Document::is_completely_loaded() const
{
    return m_completely_loaded_time.has_value();
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#completely-finish-loading
void Document::completely_finish_loading()
{
    if (!navigable())
        return;

    ScopeGuard notify_observers = [this] {
        notify_each_document_observer([&](auto const& document_observer) {
            return document_observer.document_completely_loaded();
        });
    };

    // 1. Assert: document's browsing context is non-null.
    VERIFY(browsing_context());

    // 2. Set document's completely loaded time to the current time.
    m_completely_loaded_time = AK::UnixDateTime::now();

    // NOTE: See the end of shared_declarative_refresh_steps.
    if (m_active_refresh_timer)
        m_active_refresh_timer->start();

    // 3. Let container be document's browsing context's container.
    if (!navigable()->container())
        return;

    auto container = GC::make_root(navigable()->container());

    // 4. If container is an iframe element, then queue an element task on the DOM manipulation task source given container to run the iframe load event steps given container.
    if (container && is<HTML::HTMLIFrameElement>(*container)) {
        container->queue_an_element_task(HTML::Task::Source::DOMManipulation, [container] {
            run_iframe_load_event_steps(static_cast<HTML::HTMLIFrameElement&>(*container));
        });
    }
    // 5. Otherwise, if container is non-null, then queue an element task on the DOM manipulation task source given container to fire an event named load at container.
    else if (container) {
        container->queue_an_element_task(HTML::Task::Source::DOMManipulation, [container] {
            container->dispatch_event(DOM::Event::create(container->realm(), HTML::EventNames::load));
        });
    }
}

// https://html.spec.whatwg.org/multipage/dom.html#dom-document-cookie
WebIDL::ExceptionOr<String> Document::cookie(Cookie::Source source)
{
    // On getting, if the document is a cookie-averse Document object, then the user agent must return the empty string.
    if (is_cookie_averse())
        return String {};

    // Otherwise, if the Document's origin is an opaque origin, the user agent must throw a "SecurityError" DOMException.
    if (origin().is_opaque())
        return WebIDL::SecurityError::create(realm(), "Document origin is opaque"_string);

    // Otherwise, the user agent must return the cookie-string for the document's URL for a "non-HTTP" API, decoded using
    // UTF-8 decode without BOM.
    return page().client().page_did_request_cookie(m_url, source);
}

// https://html.spec.whatwg.org/multipage/dom.html#dom-document-cookie
WebIDL::ExceptionOr<void> Document::set_cookie(StringView cookie_string, Cookie::Source source)
{
    // On setting, if the document is a cookie-averse Document object, then the user agent must do nothing.
    if (is_cookie_averse())
        return {};

    // Otherwise, if the Document's origin is an opaque origin, the user agent must throw a "SecurityError" DOMException.
    if (origin().is_opaque())
        return WebIDL::SecurityError::create(realm(), "Document origin is opaque"_string);

    // Otherwise, the user agent must act as it would when receiving a set-cookie-string for the document's URL via a
    // "non-HTTP" API, consisting of the new value encoded as UTF-8.
    if (auto cookie = Cookie::parse_cookie(url(), cookie_string); cookie.has_value())
        page().client().page_did_set_cookie(m_url, cookie.value(), source);

    return {};
}

// https://html.spec.whatwg.org/multipage/dom.html#cookie-averse-document-object
bool Document::is_cookie_averse() const
{
    // A Document object that falls into one of the following conditions is a cookie-averse Document object:

    // * A Document object whose browsing context is null.
    if (!browsing_context())
        return true;

    // * A Document whose URL's scheme is not an HTTP(S) scheme.
    if (!url().scheme().is_one_of("http"sv, "https"sv)) {
        // AD-HOC: This allows us to write cookie integration tests.
        if (!m_enable_cookies_on_file_domains || url().scheme() != "file"sv)
            return true;
    }

    return false;
}

String Document::fg_color() const
{
    if (auto* body_element = body(); body_element && !is<HTML::HTMLFrameSetElement>(*body_element))
        return body_element->get_attribute_value(HTML::AttributeNames::text);
    return ""_string;
}

void Document::set_fg_color(String const& value)
{
    if (auto* body_element = body(); body_element && !is<HTML::HTMLFrameSetElement>(*body_element))
        MUST(body_element->set_attribute(HTML::AttributeNames::text, value));
}

String Document::link_color() const
{
    if (auto* body_element = body(); body_element && !is<HTML::HTMLFrameSetElement>(*body_element))
        return body_element->get_attribute_value(HTML::AttributeNames::link);
    return ""_string;
}

void Document::set_link_color(String const& value)
{
    if (auto* body_element = body(); body_element && !is<HTML::HTMLFrameSetElement>(*body_element))
        MUST(body_element->set_attribute(HTML::AttributeNames::link, value));
}

String Document::vlink_color() const
{
    if (auto* body_element = body(); body_element && !is<HTML::HTMLFrameSetElement>(*body_element))
        return body_element->get_attribute_value(HTML::AttributeNames::vlink);
    return ""_string;
}

void Document::set_vlink_color(String const& value)
{
    if (auto* body_element = body(); body_element && !is<HTML::HTMLFrameSetElement>(*body_element))
        MUST(body_element->set_attribute(HTML::AttributeNames::vlink, value));
}

String Document::alink_color() const
{
    if (auto* body_element = body(); body_element && !is<HTML::HTMLFrameSetElement>(*body_element))
        return body_element->get_attribute_value(HTML::AttributeNames::alink);
    return ""_string;
}

void Document::set_alink_color(String const& value)
{
    if (auto* body_element = body(); body_element && !is<HTML::HTMLFrameSetElement>(*body_element))
        MUST(body_element->set_attribute(HTML::AttributeNames::alink, value));
}

String Document::bg_color() const
{
    if (auto* body_element = body(); body_element && !is<HTML::HTMLFrameSetElement>(*body_element))
        return body_element->get_attribute_value(HTML::AttributeNames::bgcolor);
    return ""_string;
}

void Document::set_bg_color(String const& value)
{
    if (auto* body_element = body(); body_element && !is<HTML::HTMLFrameSetElement>(*body_element))
        MUST(body_element->set_attribute(HTML::AttributeNames::bgcolor, value));
}

String Document::dump_dom_tree_as_json() const
{
    StringBuilder builder;
    auto json = MUST(JsonObjectSerializer<>::try_create(builder));
    serialize_tree_as_json(json);

    MUST(json.finish());
    return MUST(builder.to_string());
}

// https://html.spec.whatwg.org/multipage/semantics.html#has-a-style-sheet-that-is-blocking-scripts
bool Document::has_a_style_sheet_that_is_blocking_scripts() const
{
    // FIXME: 1. If document's script-blocking style sheet set is not empty, then return true.
    if (m_script_blocking_style_sheet_counter > 0)
        return true;

    // 2. If document's node navigable is null, then return false.
    if (!navigable())
        return false;

    // 3. Let containerDocument be document's node navigable's container document.
    auto container_document = navigable()->container_document();

    // FIXME: 4. If containerDocument is non-null and containerDocument's script-blocking style sheet set is not empty, then return true.
    if (container_document && container_document->m_script_blocking_style_sheet_counter > 0)
        return true;

    // 5. Return false
    return false;
}

String Document::referrer() const
{
    return m_referrer;
}

void Document::set_referrer(String referrer)
{
    m_referrer = move(referrer);
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#fully-active
bool Document::is_fully_active() const
{
    // A Document d is said to be fully active when d is the active document of a navigable navigable, and either
    // navigable is a top-level traversable or navigable's container document is fully active.
    auto navigable = this->navigable();
    if (!navigable)
        return false;

    auto traversable = navigable->traversable_navigable();
    if (navigable == traversable && traversable->is_top_level_traversable())
        return true;

    auto container_document = navigable->container_document();
    if (container_document && container_document != this && container_document->is_fully_active())
        return true;

    return false;
}

bool Document::is_active() const
{
    auto navigable = this->navigable();
    return navigable && navigable->active_document() == this;
}

// https://html.spec.whatwg.org/multipage/history.html#dom-document-location
GC::Ptr<HTML::Location> Document::location()
{
    // The Document object's location attribute's getter must return this Document object's relevant global object's Location object,
    // if this Document object is fully active, and null otherwise.

    if (!is_fully_active())
        return nullptr;

    return window()->location();
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-document-hidden
bool Document::hidden() const
{
    return m_visibility_state == HTML::VisibilityState::Hidden;
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-document-visibilitystate
StringView Document::visibility_state() const
{
    switch (m_visibility_state) {
    case HTML::VisibilityState::Hidden:
        return "hidden"sv;
    case HTML::VisibilityState::Visible:
        return "visible"sv;
    }
    VERIFY_NOT_REACHED();
}

void Document::set_visibility_state(Badge<HTML::BrowsingContext>, HTML::VisibilityState visibility_state)
{
    m_visibility_state = visibility_state;
}

// https://html.spec.whatwg.org/multipage/interaction.html#update-the-visibility-state
void Document::update_the_visibility_state(HTML::VisibilityState visibility_state)
{
    // 1. If document's visibility state equals visibilityState, then return.
    if (m_visibility_state == visibility_state)
        return;

    // 2. Set document's visibility state to visibilityState.
    m_visibility_state = visibility_state;

    // 3. Run any page visibility change steps which may be defined in other specifications, with visibility state and document.
    notify_each_document_observer([&](auto const& document_observer) {
        return document_observer.document_visibility_state_observer();
    },
        m_visibility_state);

    // 4. Fire an event named visibilitychange at document, with its bubbles attribute initialized to true.
    auto event = DOM::Event::create(realm(), HTML::EventNames::visibilitychange);
    event->set_bubbles(true);
    dispatch_event(event);
}

// https://drafts.csswg.org/cssom-view/#run-the-resize-steps
void Document::run_the_resize_steps()
{
    // 1. If doc’s viewport has had its width or height changed
    //    (e.g. as a result of the user resizing the browser window, or changing the page zoom scale factor,
    //    or an iframe element’s dimensions are changed) since the last time these steps were run,
    //    fire an event named resize at the Window object associated with doc.
    // 2. If the VisualViewport associated with doc has had its scale, width, or height properties changed
    //    since the last time these steps were run, fire an event named resize at the VisualViewport.

    auto viewport_size = viewport_rect().size().to_type<int>();
    bool is_initial_size = !m_last_viewport_size.has_value();

    if (m_last_viewport_size == viewport_size)
        return;
    m_last_viewport_size = viewport_size;

    if (!is_initial_size) {
        auto window_resize_event = DOM::Event::create(realm(), UIEvents::EventNames::resize);
        window_resize_event->set_is_trusted(true);
        window()->dispatch_event(window_resize_event);

        auto visual_viewport_resize_event = DOM::Event::create(realm(), UIEvents::EventNames::resize);
        visual_viewport_resize_event->set_is_trusted(true);
        visual_viewport()->dispatch_event(visual_viewport_resize_event);
    }

    schedule_layout_update();
}

// https://w3c.github.io/csswg-drafts/cssom-view-1/#document-run-the-scroll-steps
void Document::run_the_scroll_steps()
{
    // 1. For each item target in doc’s pending scroll event targets, in the order they were added to the list, run these substeps:
    for (auto& target : m_pending_scroll_event_targets) {
        // 1. If target is a Document, fire an event named scroll that bubbles at target and fire an event named scroll at the VisualViewport that is associated with target.
        if (is<Document>(*target)) {
            auto event = DOM::Event::create(realm(), HTML::EventNames::scroll);
            event->set_bubbles(true);
            target->dispatch_event(event);
            // FIXME: Fire at the associated VisualViewport
        }
        // 2. Otherwise, fire an event named scroll at target.
        else {
            auto event = DOM::Event::create(realm(), HTML::EventNames::scroll);
            target->dispatch_event(event);
        }
    }

    // 2. Empty doc’s pending scroll event targets.
    m_pending_scroll_event_targets.clear();
}

void Document::add_media_query_list(GC::Ref<CSS::MediaQueryList> media_query_list)
{
    m_media_query_lists.append(*media_query_list);
}

// https://drafts.csswg.org/cssom-view/#evaluate-media-queries-and-report-changes
void Document::evaluate_media_queries_and_report_changes()
{
    // NOTE: Not in the spec, but we take this opportunity to prune null WeakPtrs.
    m_media_query_lists.remove_all_matching([](auto& it) {
        return it.is_null();
    });

    // 1. For each MediaQueryList object target that has doc as its document,
    //    in the order they were created, oldest first, run these substeps:
    for (auto& media_query_list_ptr : m_media_query_lists) {
        // 1. If target’s matches state has changed since the last time these steps
        //    were run, fire an event at target using the MediaQueryListEvent constructor,
        //    with its type attribute initialized to change, its isTrusted attribute
        //    initialized to true, its media attribute initialized to target’s media,
        //    and its matches attribute initialized to target’s matches state.
        if (media_query_list_ptr.is_null())
            continue;
        GC::Ptr<CSS::MediaQueryList> media_query_list = media_query_list_ptr.ptr();
        bool did_match = media_query_list->matches();
        bool now_matches = media_query_list->evaluate();

        auto did_change_internally = media_query_list->has_changed_state();
        media_query_list->set_has_changed_state(false);

        if (did_change_internally == true || did_match != now_matches) {
            CSS::MediaQueryListEventInit init;
            init.media = media_query_list->media();
            init.matches = now_matches;
            auto event = CSS::MediaQueryListEvent::create(realm(), HTML::EventNames::change, init);
            event->set_is_trusted(true);
            media_query_list->dispatch_event(*event);
        }
    }

    // Also not in the spec, but this is as good a place as any to evaluate @media rules!
    evaluate_media_rules();
}

void Document::evaluate_media_rules()
{
    auto window = this->window();
    if (!window)
        return;

    bool any_media_queries_changed_match_state = false;
    for_each_active_css_style_sheet([&](CSS::CSSStyleSheet& style_sheet, auto) {
        if (style_sheet.evaluate_media_queries(*window))
            any_media_queries_changed_match_state = true;
    });

    if (any_media_queries_changed_match_state) {
        style_computer().invalidate_rule_cache();
        invalidate_style(StyleInvalidationReason::MediaQueryChangedMatchState);
    }
}

DOMImplementation* Document::implementation()
{
    if (!m_implementation)
        m_implementation = DOMImplementation::create(*this);
    return m_implementation;
}

bool Document::has_focus() const
{
    // FIXME: Return whether we actually have focus.
    return true;
}

// https://html.spec.whatwg.org/multipage/interaction.html#allow-focus-steps
bool Document::allow_focus() const
{
    // The allow focus steps, given a Document object target, are as follows:

    // 1. If target is allowed to use the "focus-without-user-activation" feature, then return true.
    if (is_allowed_to_use_feature(PolicyControlledFeature::FocusWithoutUserActivation))
        return true;

    // FIXME: 2. If any of the following are true:
    //    - target's relevant global object has transient user activation; or
    //    - target's node navigable's container, if any, is marked as locked for focus,
    //    then return true.

    // 3. Return false.
    return false;
}

void Document::set_parser(Badge<HTML::HTMLParser>, HTML::HTMLParser& parser)
{
    m_parser = parser;
}

void Document::detach_parser(Badge<HTML::HTMLParser>)
{
    m_parser = nullptr;
}

// https://www.w3.org/TR/xml/#NT-NameStartChar
static bool is_valid_name_start_character(u32 code_point)
{
    return code_point == ':'
        || (code_point >= 'A' && code_point <= 'Z')
        || code_point == '_'
        || (code_point >= 'a' && code_point <= 'z')
        || (code_point >= 0xc0 && code_point <= 0xd6)
        || (code_point >= 0xd8 && code_point <= 0xf6)
        || (code_point >= 0xf8 && code_point <= 0x2ff)
        || (code_point >= 0x370 && code_point <= 0x37d)
        || (code_point >= 0x37f && code_point <= 0x1fff)
        || (code_point >= 0x200c && code_point <= 0x200d)
        || (code_point >= 0x2070 && code_point <= 0x218f)
        || (code_point >= 0x2c00 && code_point <= 0x2fef)
        || (code_point >= 0x3001 && code_point <= 0xD7ff)
        || (code_point >= 0xf900 && code_point <= 0xfdcf)
        || (code_point >= 0xfdf0 && code_point <= 0xfffd)
        || (code_point >= 0x10000 && code_point <= 0xeffff);
}

// https://www.w3.org/TR/xml/#NT-NameChar
static inline bool is_valid_name_character(u32 code_point)
{
    return is_valid_name_start_character(code_point)
        || code_point == '-'
        || code_point == '.'
        || (code_point >= '0' && code_point <= '9')
        || code_point == 0xb7
        || (code_point >= 0x300 && code_point <= 0x36f)
        || (code_point >= 0x203f && code_point <= 0x2040);
}

// https://www.w3.org/TR/xml/#NT-Name
bool Document::is_valid_name(String const& name)
{
    if (name.is_empty())
        return false;
    auto code_points = name.code_points();
    auto it = code_points.begin();

    if (!is_valid_name_start_character(*it))
        return false;
    ++it;

    for (; it != code_points.end(); ++it) {
        if (!is_valid_name_character(*it))
            return false;
    }

    return true;
}

// https://dom.spec.whatwg.org/#validate
WebIDL::ExceptionOr<Document::PrefixAndTagName> Document::validate_qualified_name(JS::Realm& realm, FlyString const& qualified_name)
{
    if (qualified_name.is_empty())
        return WebIDL::InvalidCharacterError::create(realm, "Empty string is not a valid qualified name."_string);

    auto utf8view = qualified_name.code_points();

    Optional<size_t> colon_offset;

    bool at_start_of_name = true;

    for (auto it = utf8view.begin(); it != utf8view.end(); ++it) {
        auto code_point = *it;
        if (code_point == ':') {
            if (colon_offset.has_value())
                return WebIDL::InvalidCharacterError::create(realm, "More than one colon (:) in qualified name."_string);
            colon_offset = utf8view.byte_offset_of(it);
            at_start_of_name = true;
            continue;
        }
        if (at_start_of_name) {
            if (!is_valid_name_start_character(code_point))
                return WebIDL::InvalidCharacterError::create(realm, "Invalid start of qualified name."_string);
            at_start_of_name = false;
            continue;
        }
        if (!is_valid_name_character(code_point))
            return WebIDL::InvalidCharacterError::create(realm, "Invalid character in qualified name."_string);
    }

    if (!colon_offset.has_value())
        return Document::PrefixAndTagName {
            .prefix = {},
            .tag_name = qualified_name,
        };

    if (*colon_offset == 0)
        return WebIDL::InvalidCharacterError::create(realm, "Qualified name can't start with colon (:)."_string);

    if (*colon_offset >= (qualified_name.bytes_as_string_view().length() - 1))
        return WebIDL::InvalidCharacterError::create(realm, "Qualified name can't end with colon (:)."_string);

    return Document::PrefixAndTagName {
        .prefix = MUST(FlyString::from_utf8(qualified_name.bytes_as_string_view().substring_view(0, *colon_offset))),
        .tag_name = MUST(FlyString::from_utf8(qualified_name.bytes_as_string_view().substring_view(*colon_offset + 1))),
    };
}

// https://dom.spec.whatwg.org/#dom-document-createnodeiterator
GC::Ref<NodeIterator> Document::create_node_iterator(Node& root, unsigned what_to_show, GC::Ptr<NodeFilter> filter)
{
    return NodeIterator::create(realm(), root, what_to_show, filter);
}

// https://dom.spec.whatwg.org/#dom-document-createtreewalker
GC::Ref<TreeWalker> Document::create_tree_walker(Node& root, unsigned what_to_show, GC::Ptr<NodeFilter> filter)
{
    return TreeWalker::create(realm(), root, what_to_show, filter);
}

void Document::register_node_iterator(Badge<NodeIterator>, NodeIterator& node_iterator)
{
    auto result = m_node_iterators.set(&node_iterator);
    VERIFY(result == AK::HashSetResult::InsertedNewEntry);
}

void Document::unregister_node_iterator(Badge<NodeIterator>, NodeIterator& node_iterator)
{
    bool was_removed = m_node_iterators.remove(&node_iterator);
    VERIFY(was_removed);
}

void Document::register_document_observer(Badge<DocumentObserver>, DocumentObserver& document_observer)
{
    auto result = m_document_observers.set(document_observer);
    VERIFY(result == AK::HashSetResult::InsertedNewEntry);
}

void Document::unregister_document_observer(Badge<DocumentObserver>, DocumentObserver& document_observer)
{
    bool was_removed = m_document_observers.remove(document_observer);
    VERIFY(was_removed);
}

void Document::increment_number_of_things_delaying_the_load_event(Badge<DocumentLoadEventDelayer>)
{
    ++m_number_of_things_delaying_the_load_event;

    page().client().page_did_update_resource_count(m_number_of_things_delaying_the_load_event);
}

void Document::decrement_number_of_things_delaying_the_load_event(Badge<DocumentLoadEventDelayer>)
{
    VERIFY(m_number_of_things_delaying_the_load_event);
    --m_number_of_things_delaying_the_load_event;

    page().client().page_did_update_resource_count(m_number_of_things_delaying_the_load_event);
}

bool Document::anything_is_delaying_the_load_event() const
{
    if (m_number_of_things_delaying_the_load_event > 0)
        return true;

    for (auto& navigable : descendant_navigables()) {
        if (navigable->container()->currently_delays_the_load_event())
            return true;
    }

    // FIXME: Track down anything else that is supposed to delay the load event.

    return false;
}

void Document::set_page_showing(bool page_showing)
{
    if (m_page_showing == page_showing)
        return;

    m_page_showing = page_showing;

    notify_each_document_observer([&](auto const& document_observer) {
        return document_observer.document_page_showing_observer();
    },
        m_page_showing);
}

void Document::invalidate_stacking_context_tree()
{
    if (auto* paintable_box = this->paintable_box())
        paintable_box->invalidate_stacking_context();
}

void Document::check_favicon_after_loading_link_resource()
{
    // https://html.spec.whatwg.org/multipage/links.html#rel-icon
    // NOTE: firefox also load favicons outside the head tag, which is against spec (see table 4.6.7)
    auto* head_element = head();
    if (!head_element)
        return;

    auto favicon_link_elements = HTMLCollection::create(*head_element, HTMLCollection::Scope::Descendants, [](Element const& element) {
        if (!is<HTML::HTMLLinkElement>(element))
            return false;

        return static_cast<HTML::HTMLLinkElement const&>(element).has_loaded_icon();
    });

    if (favicon_link_elements->length() == 0) {
        dbgln_if(SPAM_DEBUG, "No favicon found to be used");
        return;
    }

    // 4.6.7.8 Link type "icon"
    //
    // If there are multiple equally appropriate icons, user agents must use the last one declared
    // in tree order at the time that the user agent collected the list of icons.
    //
    // If multiple icons are provided, the user agent must select the most appropriate icon
    // according to the type, media, and sizes attributes.
    //
    // FIXME: There is no selective behavior yet for favicons.
    for (auto i = favicon_link_elements->length(); i-- > 0;) {
        auto favicon_element = favicon_link_elements->item(i);

        if (favicon_element == m_active_element.ptr())
            return;

        // If the user agent tries to use an icon but that icon is determined, upon closer examination,
        // to in fact be inappropriate (...), then the user agent must try the next-most-appropriate icon
        // as determined by the attributes.
        if (static_cast<HTML::HTMLLinkElement*>(favicon_element)->load_favicon_and_use_if_window_is_active()) {
            m_active_favicon = favicon_element;
            return;
        }
    }

    dbgln_if(SPAM_DEBUG, "No favicon found to be used");
}

void Document::set_window(HTML::Window& window)
{
    m_window = &window;
}

// https://html.spec.whatwg.org/multipage/custom-elements.html#look-up-a-custom-element-definition
GC::Ptr<HTML::CustomElementDefinition> Document::lookup_custom_element_definition(Optional<FlyString> const& namespace_, FlyString const& local_name, Optional<String> const& is) const
{
    // 1. If namespace is not the HTML namespace, then return null.
    if (namespace_ != Namespace::HTML)
        return nullptr;

    // 2. If document's browsing context is null, then return null.
    if (!browsing_context())
        return nullptr;

    // 3. Let registry be document's relevant global object's custom element registry.
    auto registry = as<HTML::Window>(relevant_global_object(*this)).custom_elements();

    // 4. If registry's custom element definition set contains an item with name and local name both equal to localName, then return that item.
    auto converted_local_name = local_name.to_string();
    auto maybe_definition = registry->get_definition_with_name_and_local_name(converted_local_name, converted_local_name);
    if (maybe_definition)
        return maybe_definition;

    // 5. If registry's custom element definition set contains an item with name equal to is and local name equal to localName, then return that item.
    // 6. Return null.

    // NOTE: If `is` has no value, it can never match as custom element definitions always have a name and localName (i.e. not stored as Optional<String>)
    if (!is.has_value())
        return nullptr;

    return registry->get_definition_with_name_and_local_name(is.value(), converted_local_name);
}

CSS::StyleSheetList& Document::style_sheets()
{
    if (!m_style_sheets)
        m_style_sheets = CSS::StyleSheetList::create(*this);
    return *m_style_sheets;
}

CSS::StyleSheetList const& Document::style_sheets() const
{
    return const_cast<Document*>(this)->style_sheets();
}

GC::Ref<HTML::History> Document::history()
{
    if (!m_history)
        m_history = HTML::History::create(realm(), *this);
    return *m_history;
}

GC::Ref<HTML::History> Document::history() const
{
    return const_cast<Document*>(this)->history();
}

// https://html.spec.whatwg.org/multipage/origin.html#dom-document-domain
String Document::domain() const
{
    // 1. Let effectiveDomain be this's origin's effective domain.
    auto effective_domain = origin().effective_domain();

    // 2. If effectiveDomain is null, then return the empty string.
    if (!effective_domain.has_value())
        return String {};

    // 3. Return effectiveDomain, serialized.
    return effective_domain->serialize();
}

void Document::set_domain(String const& domain)
{
    dbgln("(STUBBED) Document::set_domain(domain='{}')", domain);
}

void Document::set_navigation_id(Optional<String> navigation_id)
{
    m_navigation_id = move(navigation_id);
}

Optional<String> Document::navigation_id() const
{
    return m_navigation_id;
}

HTML::SandboxingFlagSet Document::active_sandboxing_flag_set() const
{
    return m_active_sandboxing_flag_set;
}

void Document::set_active_sandboxing_flag_set(HTML::SandboxingFlagSet sandboxing_flag_set)
{
    m_active_sandboxing_flag_set = sandboxing_flag_set;
}

GC::Ref<HTML::PolicyContainer> Document::policy_container() const
{
    auto& realm = this->realm();
    if (!m_policy_container) {
        m_policy_container = realm.create<HTML::PolicyContainer>(realm);
    }
    return *m_policy_container;
}

void Document::set_policy_container(GC::Ref<HTML::PolicyContainer> policy_container)
{
    m_policy_container = policy_container;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#snapshotting-source-snapshot-params
GC::Ref<HTML::SourceSnapshotParams> Document::snapshot_source_snapshot_params() const
{
    auto& realm = this->realm();

    // To snapshot source snapshot params given a Document sourceDocument, return a new source snapshot params with
    return realm.create<HTML::SourceSnapshotParams>(
        // has transient activation
        //    true if sourceDocument's relevant global object has transient activation; otherwise false
        as<HTML::Window>(HTML::relevant_global_object(*this)).has_transient_activation(),

        // sandboxing flags
        //     sourceDocument's active sandboxing flag set
        m_active_sandboxing_flag_set,

        // allows downloading
        //     false if sourceDocument's active sandboxing flag set has the sandboxed downloads browsing context flag set; otherwise true
        !has_flag(m_active_sandboxing_flag_set, HTML::SandboxingFlagSet::SandboxedDownloads),

        // fetch client
        //     sourceDocument's relevant settings object
        relevant_settings_object(),

        // source policy container
        //     a clone of sourceDocument's policy container
        policy_container()->clone(realm));
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#descendant-navigables
Vector<GC::Root<HTML::Navigable>> Document::descendant_navigables()
{
    // 1. Let navigables be new list.
    Vector<GC::Root<HTML::Navigable>> navigables;

    // 2. Let navigableContainers be a list of all shadow-including descendants of document that are navigable containers, in shadow-including tree order.
    // 3. For each navigableContainer of navigableContainers:
    for_each_shadow_including_descendant([&](DOM::Node& node) {
        if (is<HTML::NavigableContainer>(node)) {
            auto& navigable_container = static_cast<HTML::NavigableContainer&>(node);
            // 1. If navigableContainer's content navigable is null, then continue.
            if (!navigable_container.content_navigable())
                return TraversalDecision::Continue;

            // 2. Extend navigables with navigableContainer's content navigable's active document's inclusive descendant navigables.
            auto document = navigable_container.content_navigable()->active_document();
            // AD-HOC: If the descendant navigable doesn't have an active document, just skip over it.
            if (!document)
                return TraversalDecision::Continue;
            navigables.extend(document->inclusive_descendant_navigables());
        }
        return TraversalDecision::Continue;
    });

    // 4. Return navigables.
    return navigables;
}

Vector<GC::Root<HTML::Navigable>> const Document::descendant_navigables() const
{
    return const_cast<Document&>(*this).descendant_navigables();
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#inclusive-descendant-navigables
Vector<GC::Root<HTML::Navigable>> Document::inclusive_descendant_navigables()
{
    // 1. Let navigables be « document's node navigable ».
    Vector<GC::Root<HTML::Navigable>> navigables;
    navigables.append(*navigable());

    // 2. Extend navigables with document's descendant navigables.
    navigables.extend(descendant_navigables());

    // 3. Return navigables.
    return navigables;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#ancestor-navigables
Vector<GC::Root<HTML::Navigable>> Document::ancestor_navigables()
{
    // NOTE: This isn't in the spec, but if we don't have a navigable, we can't have ancestors either.
    auto document_node_navigable = this->navigable();
    if (!document_node_navigable)
        return {};

    // 1. Let navigable be document's node navigable's parent.
    auto navigable = document_node_navigable->parent();

    // 2. Let ancestors be an empty list.
    Vector<GC::Root<HTML::Navigable>> ancestors;

    // 3. While navigable is not null:
    while (navigable) {
        // 1. Prepend navigable to ancestors.
        ancestors.prepend(*navigable);

        // 2. Set navigable to navigable's parent.
        navigable = navigable->parent();
    }

    // 4. Return ancestors.
    return ancestors;
}

Vector<GC::Root<HTML::Navigable>> const Document::ancestor_navigables() const
{
    return const_cast<Document&>(*this).ancestor_navigables();
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#inclusive-ancestor-navigables
Vector<GC::Root<HTML::Navigable>> Document::inclusive_ancestor_navigables()
{
    // 1. Let navigables be document's ancestor navigables.
    auto navigables = ancestor_navigables();

    // 2. Append document's node navigable to navigables.
    navigables.append(*navigable());

    // 3. Return navigables.
    return navigables;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#document-tree-child-navigables
Vector<GC::Root<HTML::Navigable>> Document::document_tree_child_navigables()
{
    // 1. If document's node navigable is null, then return the empty list.
    if (!navigable())
        return {};

    // 2. Let navigables be new list.
    Vector<GC::Root<HTML::Navigable>> navigables;

    // 3. Let navigableContainers be a list of all descendants of document that are navigable containers, in tree order.
    // 4. For each navigableContainer of navigableContainers:
    for_each_in_subtree_of_type<HTML::NavigableContainer>([&](HTML::NavigableContainer& navigable_container) {
        // 1. If navigableContainer's content navigable is null, then continue.
        if (!navigable_container.content_navigable())
            return TraversalDecision::Continue;
        // 2. Append navigableContainer's content navigable to navigables.
        navigables.append(*navigable_container.content_navigable());
        return TraversalDecision::Continue;
    });

    // 5. Return navigables.
    return navigables;
}

// https://html.spec.whatwg.org/multipage/document-lifecycle.html#unloading-document-cleanup-steps
void Document::run_unloading_cleanup_steps()
{
    // 1. Let window be document's relevant global object.
    auto& window = as<HTML::WindowOrWorkerGlobalScopeMixin>(HTML::relevant_global_object(*this));

    // 2. For each WebSocket object webSocket whose relevant global object is window, make disappear webSocket.
    //    If this affected any WebSocket objects, then set document's salvageable state to false.
    auto affected_any_web_sockets = window.make_disappear_all_web_sockets();
    if (affected_any_web_sockets == HTML::WindowOrWorkerGlobalScopeMixin::AffectedAnyWebSockets::Yes)
        m_salvageable = false;

    // FIXME: 3. For each WebTransport object transport whose relevant global object is window, run the context cleanup steps given transport.

    // 4. If document's salvageable state is false, then:
    if (!m_salvageable) {
        // 1. For each EventSource object eventSource whose relevant global object is equal to window, forcibly close eventSource.
        window.forcibly_close_all_event_sources();

        // 2. Clear window's map of active timers.
        window.clear_map_of_active_timers();
    }

    FileAPI::run_unloading_cleanup_steps(*this);
}

// https://html.spec.whatwg.org/multipage/document-lifecycle.html#destroy-a-document
void Document::destroy()
{
    // FIXME: 1. Assert: this is running as part of a task queued on document's relevant agent's event loop.

    // 2. Abort document.
    abort();

    // 3. Set document's salvageable state to false.
    m_salvageable = false;

    // 4. Let ports be the list of MessagePorts whose relevant global object's associated Document is document.
    // 5. For each port in ports, disentangle port.
    HTML::MessagePort::for_each_message_port([&](HTML::MessagePort& port) {
        auto& global = HTML::relevant_global_object(port);
        if (!is<HTML::Window>(global))
            return;

        auto& window = static_cast<HTML::Window&>(global);
        if (&window.associated_document() == this)
            port.disentangle();
    });

    // 6. Run any unloading document cleanup steps for document that are defined by this specification and other applicable specifications.
    run_unloading_cleanup_steps();

    // 7. Remove any tasks whose document is document from any task queue (without running those tasks).
    HTML::main_thread_event_loop().task_queue().remove_tasks_matching([this](auto& task) {
        return task.document() == this;
    });

    // AD-HOC: Mark this document as destroyed. This makes any tasks scheduled for this document in the
    //         future immediately runnable instead of blocking on the document becoming fully active.
    //         This is important because otherwise those tasks will get stuck in the task queue forever.
    m_has_been_destroyed = true;

    // 8. Set document's browsing context to null.
    m_browsing_context = nullptr;

    // Not in the spec:
    for (auto& navigable_container : HTML::NavigableContainer::all_instances()) {
        if (&navigable_container->document() == this && navigable_container->content_navigable())
            HTML::all_navigables().remove(*navigable_container->content_navigable());
    }

    // 9. Set document's node navigable's active session history entry's document state's document to null.
    if (auto navigable = this->navigable()) {
        navigable->active_session_history_entry()->document_state()->set_document(nullptr);

        // AD-HOC: We set the page's focused navigable during mouse-down events. If that navigable is this document's
        //         navigable, we must be sure to reset the page's focused navigable.
        page().navigable_document_destroyed({}, *navigable);
    }

    // FIXME: 10. Remove document from the owner set of each WorkerGlobalScope object whose set contains document.
    // FIXME: 11. For each workletGlobalScope in document's worklet global scopes, terminate workletGlobalScope.
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#make-document-unsalvageable
void Document::make_unsalvageable([[maybe_unused]] String reason)
{
    // FIXME: 1. Let details be a new not restored reason details whose reason is reason.
    // FIXME: 2. Append details to document's bfcache blocking details.

    // 3. Set document's salvageable state to false.
    set_salvageable(false);
}

// https://html.spec.whatwg.org/multipage/document-lifecycle.html#destroy-a-document-and-its-descendants
void Document::destroy_a_document_and_its_descendants(GC::Ptr<GC::Function<void()>> after_all_destruction)
{
    // 1. If document is not fully active, then:
    if (!is_fully_active()) {
        // 1. Make document unsalvageable given document and "masked".
        make_unsalvageable("masked"_string);

        // FIXME: 2. If document's node navigable is a top-level traversable,
        //           build not restored reasons for a top-level traversable and its descendants given document's node navigable.
    }

    // 2. Let childNavigables be document's child navigables.
    IGNORE_USE_IN_ESCAPING_LAMBDA auto child_navigables = document_tree_child_navigables();

    // 3. Let numberDestroyed be 0.
    IGNORE_USE_IN_ESCAPING_LAMBDA size_t number_destroyed = 0;

    // 4. For each childNavigable of childNavigables, queue a global task on the navigation and traversal task source
    //    given childNavigable's active window to perform the following steps:
    for (auto& child_navigable : child_navigables) {
        HTML::queue_global_task(HTML::Task::Source::NavigationAndTraversal, *child_navigable->active_window(), GC::create_function(heap(), [&heap = heap(), &number_destroyed, child_navigable = child_navigable.ptr()] {
            // 1. Let incrementDestroyed be an algorithm step which increments numberDestroyed.
            auto increment_destroyed = GC::create_function(heap, [&number_destroyed] { ++number_destroyed; });

            // 2. Destroy a document and its descendants given childNavigable's active document and incrementDestroyed.
            child_navigable->active_document()->destroy_a_document_and_its_descendants(move(increment_destroyed));
        }));
    }

    // 5. Wait until numberDestroyed equals childNavigable's size.
    HTML::main_thread_event_loop().spin_until(GC::create_function(heap(), [&] {
        return number_destroyed == child_navigables.size();
    }));

    // 6. Queue a global task on the navigation and traversal task source given document's relevant global object to perform the following steps:
    HTML::queue_global_task(HTML::Task::Source::NavigationAndTraversal, relevant_global_object(*this), GC::create_function(heap(), [after_all_destruction = move(after_all_destruction), this] {
        // 1. Destroy document.
        destroy();

        // 2. If afterAllDestruction was given, then run it.
        if (after_all_destruction)
            after_all_destruction->function()();
    }));
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#abort-a-document
void Document::abort()
{
    // 1. Assert: this is running as part of a task queued on document's relevant agent's event loop.

    // FIXME: 2. Cancel any instances of the fetch algorithm in the context of document,
    //           discarding any tasks queued for them, and discarding any further data received from the network for them.
    //           If this resulted in any instances of the fetch algorithm being canceled
    //           or any queued tasks or any network data getting discarded,
    //           then set document's salvageable state to false.

    // 3. If document's during-loading navigation ID for WebDriver BiDi is non-null, then:
    if (m_navigation_id.has_value()) {
        // 1. FIXME: Invoke WebDriver BiDi navigation aborted with document's node navigable,
        //           and new WebDriver BiDi navigation status whose whose id is document's navigation id,
        //           status is "canceled", and url is document's URL.

        // 2. Set document's during-loading navigation ID for WebDriver BiDi to null.
        m_navigation_id = {};
    }

    // 4. If document has an active parser, then:
    if (auto parser = active_parser()) {
        // 1. Set document's active parser was aborted to true.
        m_active_parser_was_aborted = true;

        // 2. Abort that parser.
        parser->abort();

        // 3. Set document's salvageable state to false.
        m_salvageable = false;
    }
}

// https://html.spec.whatwg.org/multipage/document-lifecycle.html#abort-a-document-and-its-descendants
void Document::abort_a_document_and_its_descendants()
{
    // FIXME 1. Assert: this is running as part of a task queued on document's relevant agent's event loop.

    // 2. Let descendantNavigables be document's descendant navigables.
    auto descendant_navigables = this->descendant_navigables();

    // 3. For each descendantNavigable of descendantNavigables, queue a global task on the navigation and traversal task source given descendantNavigable's active window to perform the following steps:
    for (auto& descendant_navigable : descendant_navigables) {
        HTML::queue_global_task(HTML::Task::Source::NavigationAndTraversal, *descendant_navigable->active_window(), GC::create_function(heap(), [this, descendant_navigable = descendant_navigable.ptr()] {
            // NOTE: This is not in the spec but we need to abort ongoing navigations in all descendant navigables.
            //       See https://github.com/whatwg/html/issues/9711
            descendant_navigable->set_ongoing_navigation({});

            // 1. Abort descendantNavigable's active document.
            descendant_navigable->active_document()->abort();

            // 2. If descendantNavigable's active document's salvageable is false, then set document's salvageable to false.
            if (!descendant_navigable->active_document()->m_salvageable)
                m_salvageable = false;
        }));
    }

    // 4. Abort document.
    abort();
}

// https://html.spec.whatwg.org/multipage/dom.html#active-parser
GC::Ptr<HTML::HTMLParser> Document::active_parser()
{
    if (!m_parser)
        return nullptr;

    if (m_parser->aborted() || m_parser->stopped())
        return nullptr;

    return m_parser;
}

void Document::set_browsing_context(HTML::BrowsingContext* browsing_context)
{
    m_browsing_context = browsing_context;
}

// https://html.spec.whatwg.org/multipage/document-lifecycle.html#unload-a-document
void Document::unload(GC::Ptr<Document>)
{
    // FIXME: 1. Assert: this is running as part of a task queued on oldDocument's event loop.

    // FIXME: 2. Let unloadTimingInfo be a new document unload timing info.

    // FIXME: 3. If newDocument is not given, then set unloadTimingInfo to null.

    // FIXME: 4. Otherwise, if newDocument's event loop is not oldDocument's event loop, then the user agent may be unloading oldDocument in parallel. In that case, the user agent should
    //           set unloadTimingInfo to null.

    // 5. Let intendToStoreInBfcache be true if the user agent intends to keep oldDocument alive in a session history entry, such that it can later be used for history traversal.
    auto intend_to_store_in_bfcache = false;

    // 6. Let eventLoop be oldDocument's relevant agent's event loop.
    auto& event_loop = *HTML::relevant_agent(*this).event_loop;

    // 7. Increase eventLoop's termination nesting level by 1.
    event_loop.increment_termination_nesting_level();

    // 8. Increase oldDocument's unload counter by 1.
    m_unload_counter += 1;

    // 9. If intendToKeepInBfcache is false, then set oldDocument's salvageable state to false.
    if (!intend_to_store_in_bfcache) {
        m_salvageable = false;
    }

    // 10. If oldDocument's page showing is true:
    if (m_page_showing) {
        // 1. Set oldDocument's page showing to false.
        m_page_showing = false;

        // 2. Fire a page transition event named pagehide at oldDocument's relevant global object with oldDocument's salvageable state.
        as<HTML::Window>(relevant_global_object(*this)).fire_a_page_transition_event(HTML::EventNames::pagehide, m_salvageable);

        // 3. Update the visibility state of oldDocument to "hidden".
        update_the_visibility_state(HTML::VisibilityState::Hidden);
    }

    // FIXME: 11. If unloadTimingInfo is not null, then set unloadTimingInfo's unload event start time to the current high resolution time given newDocument's relevant global object, coarsened
    //            given oldDocument's relevant settings object's cross-origin isolated capability.

    // 12. If oldDocument's salvageable state is false, then fire an event named unload at oldDocument's relevant global object, with legacy target override flag set.
    if (!m_salvageable) {
        // then fire an event named unload at document's relevant global object, with legacy target override flag set.
        // FIXME: The legacy target override flag is currently set by a virtual override of dispatch_event()
        //        We should reorganize this so that the flag appears explicitly here instead.
        auto event = DOM::Event::create(realm(), HTML::EventNames::unload);
        as<HTML::Window>(relevant_global_object(*this)).dispatch_event(event);
    }

    // FIXME: 13. If unloadTimingInfo is not null, then set unloadTimingInfo's unload event end time to the current high resolution time given newDocument's relevant global object, coarsened
    //            given oldDocument's relevant settings object's cross-origin isolated capability.

    // 14. Decrease eventLoop's termination nesting level by 1.
    event_loop.decrement_termination_nesting_level();

    // FIXME: 15. Set oldDocument's suspension time to the current high resolution time given document's relevant global object.

    // FIXME: 16. Set oldDocument's suspended timer handles to the result of getting the keys for the map of active timers.

    // FIXME: 17. Set oldDocument's has been scrolled by the user to false.

    // FIXME: 18. Run any unloading document cleanup steps for oldDocument that are defined by this specification and other applicable specifications.

    // 19. If oldDocument's salvageable state is false, then destroy oldDocument.
    if (!m_salvageable) {
        // NOTE: Document is destroyed from Document::unload_a_document_and_its_descendants()
    }

    // 20. Decrease oldDocument's unload counter by 1.
    m_unload_counter -= 1;

    // FIXME: 21. If newDocument is given, newDocument's was created via cross-origin redirects is false, and newDocument's origin is the same as oldDocument's origin, then set
    //            newDocument's previous document unload timing to unloadTimingInfo.

    did_stop_being_active_document_in_navigable();
}

// https://html.spec.whatwg.org/multipage/document-lifecycle.html#unload-a-document-and-its-descendants
void Document::unload_a_document_and_its_descendants(GC::Ptr<Document> new_document, GC::Ptr<GC::Function<void()>> after_all_unloads)
{
    // Specification defines this algorithm in the following steps:
    // 1. Recursively unload (and destroy) documents in descendant navigables
    // 2. Unload (and destroy) this document.
    //
    // Implementation of the spec will fail in the following scenario:
    // 1. Unload iframe's (has attribute name="test") document
    //    1.1. Destroy iframe's document
    // 2. Unload iframe's parent document
    //    2.1. Dispatch "unload" event
    //       2.2. In "unload" event handler run `window["test"]`
    //          2.2.1. Execute Window::document_tree_child_navigable_target_name_property_set()
    //             2.2.1.1. Fail to access iframe's navigable active document because it was destroyed on step 1.1
    //
    // We change the algorithm to:
    // 1. Unload all descendant documents without destroying them
    // 2. Unload this document
    // 3. Destroy all descendant documents
    // 4. Destroy this document
    //
    // This way we maintain the invariant that all navigable containers present in the DOM tree
    // have an active document while the document is being unloaded.

    IGNORE_USE_IN_ESCAPING_LAMBDA size_t number_unloaded = 0;

    auto navigable = this->navigable();

    Vector<GC::Root<HTML::Navigable>> descendant_navigables;
    for (auto& other_navigable : HTML::all_navigables()) {
        if (navigable->is_ancestor_of(*other_navigable))
            descendant_navigables.append(other_navigable);
    }

    IGNORE_USE_IN_ESCAPING_LAMBDA auto unloaded_documents_count = descendant_navigables.size() + 1;

    HTML::queue_global_task(HTML::Task::Source::NavigationAndTraversal, HTML::relevant_global_object(*this), GC::create_function(heap(), [&number_unloaded, this, new_document] {
        unload(new_document);
        ++number_unloaded;
    }));

    for (auto& descendant_navigable : descendant_navigables) {
        HTML::queue_global_task(HTML::Task::Source::NavigationAndTraversal, *descendant_navigable->active_window(), GC::create_function(heap(), [&number_unloaded, descendant_navigable = descendant_navigable.ptr()] {
            descendant_navigable->active_document()->unload();
            ++number_unloaded;
        }));
    }

    HTML::main_thread_event_loop().spin_until(GC::create_function(heap(), [&] {
        return number_unloaded == unloaded_documents_count;
    }));

    destroy_a_document_and_its_descendants(move(after_all_unloads));
}

// https://html.spec.whatwg.org/multipage/iframe-embed-object.html#allowed-to-use
bool Document::is_allowed_to_use_feature(PolicyControlledFeature feature) const
{
    // 1. If document's browsing context is null, then return false.
    if (browsing_context() == nullptr)
        return false;

    // 2. If document is not fully active, then return false.
    if (!is_fully_active())
        return false;

    // 3. If the result of running is feature enabled in document for origin on feature, document, and document's origin
    //    is "Enabled", then return true.
    // FIXME: This is ad-hoc. Implement the Permissions Policy specification.
    switch (feature) {
    case PolicyControlledFeature::Autoplay:
        if (PermissionsPolicy::AutoplayAllowlist::the().is_allowed_for_origin(*this, origin()) == PermissionsPolicy::Decision::Enabled)
            return true;
        break;
    case PolicyControlledFeature::FocusWithoutUserActivation:
        // FIXME: Implement allowlist for this.
        return true;
    }

    // 4. Return false.
    return false;
}

void Document::did_stop_being_active_document_in_navigable()
{
    tear_down_layout_tree();

    notify_each_document_observer([&](auto const& document_observer) {
        return document_observer.document_became_inactive();
    });

    if (m_animation_driver_timer)
        m_animation_driver_timer->stop();
}

void Document::increment_throw_on_dynamic_markup_insertion_counter(Badge<HTML::HTMLParser>)
{
    ++m_throw_on_dynamic_markup_insertion_counter;
}

void Document::decrement_throw_on_dynamic_markup_insertion_counter(Badge<HTML::HTMLParser>)
{
    VERIFY(m_throw_on_dynamic_markup_insertion_counter);
    --m_throw_on_dynamic_markup_insertion_counter;
}

// https://html.spec.whatwg.org/multipage/scripting.html#appropriate-template-contents-owner-document
GC::Ref<DOM::Document> Document::appropriate_template_contents_owner_document()
{
    // 1. If doc is not a Document created by this algorithm, then:
    if (!created_for_appropriate_template_contents()) {
        // 1. If doc does not yet have an associated inert template document, then:
        if (!m_associated_inert_template_document) {
            // 1. Let new doc be a new Document (whose browsing context is null). This is "a Document created by this algorithm" for the purposes of the step above.
            auto new_document = HTML::HTMLDocument::create(realm());
            new_document->m_created_for_appropriate_template_contents = true;

            // 2. If doc is an HTML document, mark new doc as an HTML document also.
            if (document_type() == Type::HTML)
                new_document->set_document_type(Type::HTML);

            // 3. Let doc's associated inert template document be new doc.
            m_associated_inert_template_document = new_document;
        }
        // 2. Set doc to doc's associated inert template document.
        return *m_associated_inert_template_document;
    }
    // 2. Return doc.
    return *this;
}

String Document::dump_accessibility_tree_as_json()
{
    StringBuilder builder;
    auto accessibility_tree = AccessibilityTreeNode::create(this, nullptr);
    build_accessibility_tree(*&accessibility_tree);
    auto json = MUST(JsonObjectSerializer<>::try_create(builder));

    // Empty document
    if (!accessibility_tree->value()) {
        MUST(json.add("type"sv, "element"sv));
        MUST(json.add("role"sv, "document"sv));
    } else {
        accessibility_tree->serialize_tree_as_json(json, *this);
    }

    MUST(json.finish());
    return MUST(builder.to_string());
}

// https://dom.spec.whatwg.org/#dom-document-createattribute
WebIDL::ExceptionOr<GC::Ref<Attr>> Document::create_attribute(String const& local_name)
{
    // 1. If localName does not match the Name production in XML, then throw an "InvalidCharacterError" DOMException.
    if (!is_valid_name(local_name))
        return WebIDL::InvalidCharacterError::create(realm(), "Invalid character in attribute name."_string);

    // 2. If this is an HTML document, then set localName to localName in ASCII lowercase.
    // 3. Return a new attribute whose local name is localName and node document is this.
    return Attr::create(*this, is_html_document() ? local_name.to_ascii_lowercase() : local_name);
}

// https://dom.spec.whatwg.org/#dom-document-createattributens
WebIDL::ExceptionOr<GC::Ref<Attr>> Document::create_attribute_ns(Optional<FlyString> const& namespace_, String const& qualified_name)
{
    // 1. Let namespace, prefix, and localName be the result of passing namespace and qualifiedName to validate and extract.
    auto extracted_qualified_name = TRY(validate_and_extract(realm(), namespace_, qualified_name));

    // 2. Return a new attribute whose namespace is namespace, namespace prefix is prefix, local name is localName, and node document is this.

    return Attr::create(*this, extracted_qualified_name);
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#make-active
void Document::make_active()
{
    // 1. Let window be document's relevant global object.
    auto& window = as<HTML::Window>(HTML::relevant_global_object(*this));

    set_window(window);

    // 2. Set document's browsing context's WindowProxy's [[Window]] internal slot value to window.
    m_browsing_context->window_proxy()->set_window(window);

    if (m_browsing_context->is_top_level()) {
        page().client().page_did_change_active_document_in_top_level_browsing_context(*this);
    }

    // 3. Set document's visibility state to document's node navigable's traversable navigable's system visibility state.
    if (navigable()) {
        m_visibility_state = navigable()->traversable_navigable()->system_visibility_state();
    }

    // TODO: 4. Queue a new VisibilityStateEntry whose visibility state is document's visibility state and whose timestamp is zero.

    // 5. Set window's relevant settings object's execution ready flag.
    HTML::relevant_settings_object(window).execution_ready = true;

    if (m_needs_to_call_page_did_load) {
        navigable()->traversable_navigable()->page().client().page_did_finish_loading(url());
        m_needs_to_call_page_did_load = false;
    }

    notify_each_document_observer([&](auto const& document_observer) {
        return document_observer.document_became_active();
    });
}

HTML::ListOfAvailableImages& Document::list_of_available_images()
{
    return *m_list_of_available_images;
}

HTML::ListOfAvailableImages const& Document::list_of_available_images() const
{
    return *m_list_of_available_images;
}

CSSPixelRect Document::viewport_rect() const
{
    if (auto const navigable = this->navigable())
        return navigable->viewport_rect();
    return CSSPixelRect {};
}

GC::Ref<CSS::VisualViewport> Document::visual_viewport()
{
    if (!m_visual_viewport)
        m_visual_viewport = CSS::VisualViewport::create(*this);
    return *m_visual_viewport;
}

void Document::register_viewport_client(ViewportClient& client)
{
    auto result = m_viewport_clients.set(&client);
    VERIFY(result == AK::HashSetResult::InsertedNewEntry);
}

void Document::unregister_viewport_client(ViewportClient& client)
{
    bool was_removed = m_viewport_clients.remove(&client);
    VERIFY(was_removed);
}

void Document::inform_all_viewport_clients_about_the_current_viewport_rect()
{
    for (auto* client : m_viewport_clients)
        client->did_set_viewport_rect(viewport_rect());
}

void Document::register_intersection_observer(Badge<IntersectionObserver::IntersectionObserver>, IntersectionObserver::IntersectionObserver& observer)
{
    auto result = m_intersection_observers.set(observer);
    VERIFY(result == AK::HashSetResult::InsertedNewEntry);
}

void Document::unregister_intersection_observer(Badge<IntersectionObserver::IntersectionObserver>, IntersectionObserver::IntersectionObserver& observer)
{
    bool was_removed = m_intersection_observers.remove(observer);
    VERIFY(was_removed);
}

void Document::register_resize_observer(Badge<ResizeObserver::ResizeObserver>, ResizeObserver::ResizeObserver& observer)
{
    m_resize_observers.append(observer);
}

void Document::unregister_resize_observer(Badge<ResizeObserver::ResizeObserver>, ResizeObserver::ResizeObserver& observer)
{
    m_resize_observers.remove_first_matching([&](auto& registered_observer) {
        return registered_observer.ptr() == &observer;
    });
}

// https://www.w3.org/TR/intersection-observer/#queue-an-intersection-observer-task
void Document::queue_intersection_observer_task()
{
    auto window = this->window();
    if (!window)
        return;

    // 1. If document’s IntersectionObserverTaskQueued flag is set to true, return.
    if (m_intersection_observer_task_queued)
        return;

    // 2. Set document’s IntersectionObserverTaskQueued flag to true.
    m_intersection_observer_task_queued = true;

    // 3. Queue a task on the IntersectionObserver task source associated with the document's event loop to notify intersection observers.
    HTML::queue_global_task(HTML::Task::Source::IntersectionObserver, *window, GC::create_function(heap(), [this]() {
        auto& realm = this->realm();

        // https://www.w3.org/TR/intersection-observer/#notify-intersection-observers
        // 1. Set document’s IntersectionObserverTaskQueued flag to false.
        m_intersection_observer_task_queued = false;

        // 2. Let notify list be a list of all IntersectionObservers whose root is in the DOM tree of document.
        Vector<GC::Root<IntersectionObserver::IntersectionObserver>> notify_list;
        notify_list.try_ensure_capacity(m_intersection_observers.size()).release_value_but_fixme_should_propagate_errors();
        for (auto& observer : m_intersection_observers) {
            notify_list.append(GC::make_root(observer));
        }

        // 3. For each IntersectionObserver object observer in notify list, run these steps:
        for (auto& observer : notify_list) {
            // 2. Let queue be a copy of observer’s internal [[QueuedEntries]] slot.
            // 3. Clear observer’s internal [[QueuedEntries]] slot.
            auto queue = observer->take_records();

            // 1. If observer’s internal [[QueuedEntries]] slot is empty, continue.
            if (queue.is_empty())
                continue;

            auto wrapped_queue = MUST(JS::Array::create(realm, 0));
            for (size_t i = 0; i < queue.size(); ++i) {
                auto& record = queue.at(i);
                auto property_index = JS::PropertyKey { i };
                MUST(wrapped_queue->create_data_property(property_index, record.ptr()));
            }

            // 4. Let callback be the value of observer’s internal [[callback]] slot.
            auto& callback = observer->callback();

            // 5. Invoke callback with queue as the first argument, observer as the second argument, and observer as the callback this value. If this throws an exception, report the exception.
            // NOTE: This does not follow the spec as written precisely, but this is the same thing we do elsewhere and there is a WPT test that relies on this.
            (void)WebIDL::invoke_callback(callback, observer.ptr(), WebIDL::ExceptionBehavior::Report, wrapped_queue, observer.ptr());
        }
    }));
}

// https://www.w3.org/TR/intersection-observer/#queue-an-intersectionobserverentry
void Document::queue_an_intersection_observer_entry(IntersectionObserver::IntersectionObserver& observer, HighResolutionTime::DOMHighResTimeStamp time, GC::Ref<Geometry::DOMRectReadOnly> root_bounds, GC::Ref<Geometry::DOMRectReadOnly> bounding_client_rect, GC::Ref<Geometry::DOMRectReadOnly> intersection_rect, bool is_intersecting, double intersection_ratio, GC::Ref<Element> target)
{
    auto& realm = this->realm();

    // 1. Construct an IntersectionObserverEntry, passing in time, rootBounds, boundingClientRect, intersectionRect, isIntersecting, and target.
    auto entry = realm.create<IntersectionObserver::IntersectionObserverEntry>(realm, time, root_bounds, bounding_client_rect, intersection_rect, is_intersecting, intersection_ratio, target);

    // 2. Append it to observer’s internal [[QueuedEntries]] slot.
    observer.queue_entry({}, entry);

    // 3. Queue an intersection observer task for document.
    queue_intersection_observer_task();
}

// https://www.w3.org/TR/intersection-observer/#compute-the-intersection
static CSSPixelRect compute_intersection(GC::Ref<Element> target, IntersectionObserver::IntersectionObserver const& observer)
{
    // 1. Let intersectionRect be the result of getting the bounding box for target.
    auto intersection_rect = target->get_bounding_client_rect();

    // FIXME: 2. Let container be the containing block of target.
    // FIXME: 3. While container is not root:
    // FIXME:   1. If container is the document of a nested browsing context, update intersectionRect by clipping to
    //             the viewport of the document, and update container to be the browsing context container of container.
    // FIXME:   2. Map intersectionRect to the coordinate space of container.
    // FIXME:   3. If container has a content clip or a css clip-path property, update intersectionRect by applying
    //             container’s clip.
    // FIXME:   4. If container is the root element of a browsing context, update container to be the browsing context’s
    //             document; otherwise, update container to be the containing block of container.
    // FIXME: 4. Map intersectionRect to the coordinate space of root.

    // 5. Update intersectionRect by intersecting it with the root intersection rectangle.
    // FIXME: Pass in target so we can properly apply rootMargin.
    auto root_intersection_rectangle = observer.root_intersection_rectangle();
    intersection_rect.intersect(root_intersection_rectangle);

    // FIXME: 6. Map intersectionRect to the coordinate space of the viewport of the document containing target.

    // 7. Return intersectionRect.
    return intersection_rect;
}

// https://www.w3.org/TR/intersection-observer/#run-the-update-intersection-observations-steps
void Document::run_the_update_intersection_observations_steps(HighResolutionTime::DOMHighResTimeStamp time)
{
    auto& realm = this->realm();

    // 1. Let observer list be a list of all IntersectionObservers whose root is in the DOM tree of document.
    //    For the top-level browsing context, this includes implicit root observers.
    // 2. For each observer in observer list:

    // NOTE: We make a copy of the intersection observers list to avoid modifying it while iterating.
    GC::RootVector<GC::Ref<IntersectionObserver::IntersectionObserver>> intersection_observers(heap());
    intersection_observers.ensure_capacity(m_intersection_observers.size());
    for (auto& observer : m_intersection_observers)
        intersection_observers.append(observer);

    for (auto& observer : intersection_observers) {
        // 1. Let rootBounds be observer’s root intersection rectangle.
        auto root_bounds = observer->root_intersection_rectangle();

        // 2. For each target in observer’s internal [[ObservationTargets]] slot, processed in the same order that
        //    observe() was called on each target:
        for (auto& target : observer->observation_targets()) {
            // 1. Let:
            // thresholdIndex be 0.
            size_t threshold_index = 0;

            // isIntersecting be false.
            bool is_intersecting = false;

            // targetRect be a DOMRectReadOnly with x, y, width, and height set to 0.
            CSSPixelRect target_rect { 0, 0, 0, 0 };

            // intersectionRect be a DOMRectReadOnly with x, y, width, and height set to 0.
            CSSPixelRect intersection_rect { 0, 0, 0, 0 };

            // SPEC ISSUE: It doesn't pass in intersection ratio to "queue an IntersectionObserverEntry" despite needing it.
            //             This is default 0, as isIntersecting is default false, see step 9.
            double intersection_ratio = 0.0;

            // 2. If the intersection root is not the implicit root, and target is not in the same document as the intersection root, skip to step 11.
            // 3. If the intersection root is an Element, and target is not a descendant of the intersection root in the containing block chain, skip to step 11.
            // FIXME: Actually use the containing block chain.
            auto intersection_root = observer->intersection_root();
            auto intersection_root_document = intersection_root.visit([](auto& node) -> GC::Ref<Document> {
                return node->document();
            });
            if (!(observer->root().has<Empty>() && &target->document() == intersection_root_document.ptr())
                || !(intersection_root.has<GC::Root<DOM::Element>>() && !target->is_descendant_of(*intersection_root.get<GC::Root<DOM::Element>>()))) {
                // 4. Set targetRect to the DOMRectReadOnly obtained by getting the bounding box for target.
                target_rect = target->get_bounding_client_rect();

                // 5. Let intersectionRect be the result of running the compute the intersection algorithm on target and
                //    observer’s intersection root.
                intersection_rect = compute_intersection(target, observer);

                // 6. Let targetArea be targetRect’s area.
                auto target_area = target_rect.width() * target_rect.height();

                // 7. Let intersectionArea be intersectionRect’s area.
                auto intersection_area = intersection_rect.size().area();

                // 8. Let isIntersecting be true if targetRect and rootBounds intersect or are edge-adjacent, even if the
                //    intersection has zero area (because rootBounds or targetRect have zero area).
                is_intersecting = target_rect.edge_adjacent_intersects(root_bounds);

                // 9. If targetArea is non-zero, let intersectionRatio be intersectionArea divided by targetArea.
                //    Otherwise, let intersectionRatio be 1 if isIntersecting is true, or 0 if isIntersecting is false.
                if (target_area != 0.0)
                    intersection_ratio = (intersection_area / target_area).to_double();
                else
                    intersection_ratio = is_intersecting ? 1.0 : 0.0;

                // 10. Set thresholdIndex to the index of the first entry in observer.thresholds whose value is greater
                //     than intersectionRatio, or the length of observer.thresholds if intersectionRatio is greater than
                //     or equal to the last entry in observer.thresholds.
                threshold_index = observer->thresholds().find_first_index_if([&intersection_ratio](double threshold_value) {
                                                            return threshold_value > intersection_ratio;
                                                        })
                                      .value_or(observer->thresholds().size());
            }

            // 11. Let intersectionObserverRegistration be the IntersectionObserverRegistration record in target’s
            //     internal [[RegisteredIntersectionObservers]] slot whose observer property is equal to observer.
            auto& intersection_observer_registration = target->get_intersection_observer_registration({}, observer);

            // 12. Let previousThresholdIndex be the intersectionObserverRegistration’s previousThresholdIndex property.
            auto previous_threshold_index = intersection_observer_registration.previous_threshold_index;

            // 13. Let previousIsIntersecting be the intersectionObserverRegistration’s previousIsIntersecting property.
            auto previous_is_intersecting = intersection_observer_registration.previous_is_intersecting;

            // 14. If thresholdIndex does not equal previousThresholdIndex or if isIntersecting does not equal
            //     previousIsIntersecting, queue an IntersectionObserverEntry, passing in observer, time,
            //     rootBounds, targetRect, intersectionRect, isIntersecting, and target.
            if (threshold_index != previous_threshold_index || is_intersecting != previous_is_intersecting) {
                auto root_bounds_as_dom_rect = Geometry::DOMRectReadOnly::construct_impl(realm, static_cast<double>(root_bounds.x()), static_cast<double>(root_bounds.y()), static_cast<double>(root_bounds.width()), static_cast<double>(root_bounds.height())).release_value_but_fixme_should_propagate_errors();

                // SPEC ISSUE: It doesn't pass in intersectionRatio, but it's required.
                auto target_dom_rect = MUST(Geometry::DOMRectReadOnly::construct_impl(realm, static_cast<double>(target_rect.x()), static_cast<double>(target_rect.y()), static_cast<double>(target_rect.width()), static_cast<double>(target_rect.height())));
                auto intersection_dom_rect = MUST(Geometry::DOMRectReadOnly::construct_impl(realm, static_cast<double>(intersection_rect.x()), static_cast<double>(intersection_rect.y()), static_cast<double>(intersection_rect.width()), static_cast<double>(intersection_rect.height())));
                queue_an_intersection_observer_entry(observer, time, root_bounds_as_dom_rect, target_dom_rect, intersection_dom_rect, is_intersecting, intersection_ratio, target);
            }

            // 15. Assign thresholdIndex to intersectionObserverRegistration’s previousThresholdIndex property.
            intersection_observer_registration.previous_threshold_index = threshold_index;

            // 16. Assign isIntersecting to intersectionObserverRegistration’s previousIsIntersecting property.
            intersection_observer_registration.previous_is_intersecting = is_intersecting;
        }
    }
}

// https://html.spec.whatwg.org/multipage/urls-and-fetching.html#start-intersection-observing-a-lazy-loading-element
void Document::start_intersection_observing_a_lazy_loading_element(Element& element)
{
    VERIFY(element.is_lazy_loading());

    auto& realm = this->realm();

    // 1. Let doc be element's node document.
    VERIFY(&element.document() == this);

    // 2. If doc's lazy load intersection observer is null, set it to a new IntersectionObserver instance, initialized as follows:
    if (!m_lazy_load_intersection_observer) {
        // - The callback is these steps, with arguments entries and observer:
        auto callback = JS::NativeFunction::create(realm, ""_fly_string, [this](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            // For each entry in entries using a method of iteration which does not trigger developer-modifiable array accessors or iteration hooks:
            auto& entries = as<JS::Array>(vm.argument(0).as_object());
            auto entries_length = MUST(MUST(entries.get(vm.names.length)).to_length(vm));

            for (size_t i = 0; i < entries_length; ++i) {
                auto property_key = JS::PropertyKey { i };
                auto& entry = as<IntersectionObserver::IntersectionObserverEntry>(entries.get_without_side_effects(property_key).as_object());

                // 1. Let resumptionSteps be null.
                GC::Ptr<GC::Function<void()>> resumption_steps;

                // 2. If entry.isIntersecting is true, then set resumptionSteps to entry.target's lazy load resumption steps.
                if (entry.is_intersecting()) {
                    // 5. Set entry.target's lazy load resumption steps to null.
                    VERIFY(entry.target()->is_lazy_loading());
                    resumption_steps = entry.target()->take_lazy_load_resumption_steps({});
                }

                // 3. If resumptionSteps is null, then return.
                if (!resumption_steps) {
                    // NOTE: This is wrong in the spec, since we want to keep processing
                    //       entries even if one of them doesn't have resumption steps.
                    // FIXME: Spec bug: https://github.com/whatwg/html/issues/10019
                    continue;
                }

                // 4. Stop intersection-observing a lazy loading element for entry.target.
                stop_intersection_observing_a_lazy_loading_element(entry.target());

                // 5. Set entry.target's lazy load resumption steps to null.
                entry.target()->take_lazy_load_resumption_steps({});

                // 6. Invoke resumptionSteps.
                resumption_steps->function()();
            }

            return JS::js_undefined();
        });

        // FIXME: The options is an IntersectionObserverInit dictionary with the following dictionary members: «[ "rootMargin" → lazy load root margin ]»
        // Spec Note: This allows for fetching the image during scrolling, when it does not yet — but is about to — intersect the viewport.
        auto options = IntersectionObserver::IntersectionObserverInit {};

        auto wrapped_callback = realm.heap().allocate<WebIDL::CallbackType>(callback, realm);
        m_lazy_load_intersection_observer = IntersectionObserver::IntersectionObserver::construct_impl(realm, wrapped_callback, options).release_value_but_fixme_should_propagate_errors();
    }

    // 3. Call doc's lazy load intersection observer's observe method with element as the argument.
    VERIFY(m_lazy_load_intersection_observer);
    m_lazy_load_intersection_observer->observe(element);
}

// https://html.spec.whatwg.org/multipage/urls-and-fetching.html#stop-intersection-observing-a-lazy-loading-element
void Document::stop_intersection_observing_a_lazy_loading_element(Element& element)
{
    // 1. Let doc be element's node document.
    // NOTE: It's `this`.

    // 2. Assert: doc's lazy load intersection observer is not null.
    VERIFY(m_lazy_load_intersection_observer);

    // 3. Call doc's lazy load intersection observer unobserve method with element as the argument.
    m_lazy_load_intersection_observer->unobserve(element);
}

// https://html.spec.whatwg.org/multipage/semantics.html#shared-declarative-refresh-steps
void Document::shared_declarative_refresh_steps(StringView input, GC::Ptr<HTML::HTMLMetaElement const> meta_element)
{
    // 1. If document's will declaratively refresh is true, then return.
    if (m_will_declaratively_refresh)
        return;

    // 2. Let position point at the first code point of input.
    GenericLexer lexer(input);

    // 3. Skip ASCII whitespace within input given position.
    lexer.ignore_while(Infra::is_ascii_whitespace);

    // 4. Let time be 0.
    u32 time = 0;

    // 5. Collect a sequence of code points that are ASCII digits from input given position, and let the result be timeString.
    auto time_string = lexer.consume_while(is_ascii_digit);

    // 6. If timeString is the empty string, then:
    if (time_string.is_empty()) {
        // 1. If the code point in input pointed to by position is not U+002E (.), then return.
        if (lexer.peek() != '.')
            return;
    }

    // 7. Otherwise, set time to the result of parsing timeString using the rules for parsing non-negative integers.
    auto maybe_time = Web::HTML::parse_non_negative_integer(time_string);

    // FIXME: Since we only collected ASCII digits, this can only fail because of overflow. What do we do when that happens? For now, default to 0.
    if (maybe_time.has_value() && maybe_time.value() < NumericLimits<int>::max() && !Checked<int>::multiplication_would_overflow(static_cast<int>(maybe_time.value()), 1000)) {
        time = maybe_time.value();
    }

    // 8. Collect a sequence of code points that are ASCII digits and U+002E FULL STOP characters (.) from input given
    //    position. Ignore any collected characters.
    lexer.ignore_while([](auto c) {
        return is_ascii_digit(c) || c == '.';
    });

    // 9. Let urlRecord be document's URL.
    auto url_record = url();

    // 10. If position is not past the end of input, then:
    if (!lexer.is_eof()) {
        // 1. If the code point in input pointed to by position is not U+003B (;), U+002C (,), or ASCII whitespace, then return.
        if (lexer.peek() != ';' && lexer.peek() != ',' && !Infra::is_ascii_whitespace(lexer.peek()))
            return;

        // 2. Skip ASCII whitespace within input given position.
        lexer.ignore_while(Infra::is_ascii_whitespace);

        // 3. If the code point in input pointed to by position is U+003B (;) or U+002C (,), then advance position to the next code point.
        if (lexer.peek() == ';' || lexer.peek() == ',')
            lexer.ignore(1);

        // 4. Skip ASCII whitespace within input given position.
        lexer.ignore_while(Infra::is_ascii_whitespace);
    }

    // 11. If position is not past the end of input, then:
    if (!lexer.is_eof()) {
        // 1. Let urlString be the substring of input from the code point at position to the end of the string.
        auto url_string = lexer.remaining();

        // 2. If the code point in input pointed to by position is U+0055 (U) or U+0075 (u), then advance position to the next code point. Otherwise, jump to the step labeled skip quotes.
        if (lexer.peek() == 'U' || lexer.peek() == 'u')
            lexer.ignore(1);
        else
            goto skip_quotes;

        // 3. If the code point in input pointed to by position is U+0052 (R) or U+0072 (r), then advance position to the next code point. Otherwise, jump to the step labeled parse.
        if (lexer.peek() == 'R' || lexer.peek() == 'r')
            lexer.ignore(1);
        else
            goto parse;

        // 4. If the code point in input pointed to by position is U+004C (L) or U+006C (l), then advance position to the next code point. Otherwise, jump to the step labeled parse.
        if (lexer.peek() == 'L' || lexer.peek() == 'l')
            lexer.ignore(1);
        else
            goto parse;

        // 5. Skip ASCII whitespace within input given position.
        lexer.ignore_while(Infra::is_ascii_whitespace);

        // 6. If the code point in input pointed to by position is U+003D (=), then advance position to the next code point. Otherwise, jump to the step labeled parse.
        if (lexer.peek() == '=')
            lexer.ignore(1);
        else
            goto parse;

        // 7. Skip ASCII whitespace within input given position.
        lexer.ignore_while(Infra::is_ascii_whitespace);

    skip_quotes: {
        // 8. Skip quotes: If the code point in input pointed to by position is U+0027 (') or U+0022 ("), then let
        //    quote be that code point, and advance position to the next code point. Otherwise, let quote be the empty
        //    string.
        Optional<char> quote;
        if (lexer.peek() == '\'' || lexer.peek() == '"')
            quote = lexer.consume();

        // 9. Set urlString to the substring of input from the code point at position to the end of the string.
        // 10. If quote is not the empty string, and there is a code point in urlString equal to quote, then truncate
        //     urlString at that code point, so that it and all subsequent code points are removed.
        url_string = lexer.consume_while([&quote](auto c) {
            return !quote.has_value() || c != quote.value();
        });
    }

    parse:
        // 11. Parse: Parse urlString relative to document. If that fails, return. Otherwise, set urlRecord to the
        //     resulting URL record.
        auto maybe_url_record = parse_url(url_string);
        if (!maybe_url_record.has_value())
            return;

        url_record = maybe_url_record.release_value();
    }

    // 12. Set document's will declaratively refresh to true.
    m_will_declaratively_refresh = true;

    // 13. Perform one or more of the following steps:
    // - After the refresh has come due (as defined below), if the user has not canceled the redirect and, if meta is
    //   given, document's active sandboxing flag set does not have the sandboxed automatic features browsing context
    //   flag set, then navigate document's node navigable to urlRecord using document, with historyHandling set to
    //   "replace".
    m_active_refresh_timer = Core::Timer::create_single_shot(time * 1000, [this, has_meta_element = !!meta_element, url_record = move(url_record)]() {
        if (has_meta_element && has_flag(active_sandboxing_flag_set(), HTML::SandboxingFlagSet::SandboxedAutomaticFeatures))
            return;

        auto navigable = this->navigable();
        if (!navigable || navigable->has_been_destroyed())
            return;

        MUST(navigable->navigate({ .url = url_record, .source_document = *this, .history_handling = Bindings::NavigationHistoryBehavior::Replace }));
    });

    // For the purposes of the previous paragraph, a refresh is said to have come due as soon as the later of the
    // following two conditions occurs:

    // - At least time seconds have elapsed since document's completely loaded time, adjusted to take into
    //   account user or user agent preferences.
    // m_active_refresh_timer is started in completely_finished_loading after setting the completely loaded time.

    // - If meta is given, at least time seconds have elapsed since meta was inserted into the document document,
    // adjusted to take into account user or user agent preferences.
    // NOTE: This is only done if completely loaded time has a value because shared_declarative_refresh_steps is called
    // by HTMLMetaElement::inserted and if the document hasn't finished loading when the meta element was inserted,
    // then the document completely finishing loading will _always_ come after inserting the meta element.
    if (meta_element && m_completely_loaded_time.has_value()) {
        m_active_refresh_timer->start();
    }
}

Painting::ViewportPaintable const* Document::paintable() const
{
    return static_cast<Painting::ViewportPaintable const*>(Node::paintable());
}

Painting::ViewportPaintable* Document::paintable()
{
    return static_cast<Painting::ViewportPaintable*>(Node::paintable());
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#restore-the-history-object-state
void Document::restore_the_history_object_state(GC::Ref<HTML::SessionHistoryEntry> entry)
{
    // 1. Let targetRealm be document's relevant realm.
    auto& target_realm = HTML::relevant_realm(*this);

    // 2. Let state be StructuredDeserialize(entry's classic history API state, targetRealm). If this throws an exception, catch it and let state be null.
    // 3. Set document's history object's state to state.
    auto state_or_error = HTML::structured_deserialize(target_realm.vm(), entry->classic_history_api_state(), target_realm);
    if (state_or_error.is_error())
        m_history->set_state(JS::js_null());
    else
        m_history->set_state(state_or_error.release_value());
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#update-document-for-history-step-application
void Document::update_for_history_step_application(GC::Ref<HTML::SessionHistoryEntry> entry, bool do_not_reactivate, size_t script_history_length, size_t script_history_index, Optional<Bindings::NavigationType> navigation_type, Optional<Vector<GC::Ref<HTML::SessionHistoryEntry>>> entries_for_navigation_api, GC::Ptr<HTML::SessionHistoryEntry> previous_entry_for_activation, bool update_navigation_api)
{
    (void)previous_entry_for_activation;

    // 1. Let documentIsNew be true if document's latest entry is null; otherwise false.
    auto document_is_new = !m_latest_entry;

    // 2. Let documentsEntryChanged be true if document's latest entry is not entry; otherwise false.
    auto documents_entry_changed = m_latest_entry != entry;

    // 3. Set document's history object's index to scriptHistoryIndex.
    history()->m_index = script_history_index;

    // 4. Set document's history object's length to scriptHistoryLength.
    history()->m_length = script_history_length;

    // 5. Let navigation be history's relevant global object's navigation API.
    auto navigation = as<HTML::Window>(HTML::relevant_global_object(*this)).navigation();

    // 6. If documentsEntryChanged is true, then:
    // NOTE: documentsEntryChanged can be false for one of two reasons: either we are restoring from bfcache,
    //      or we are asynchronously finishing up a synchronous navigation which already synchronously set document's latest entry.
    //      The doNotReactivate argument distinguishes between these two cases.
    if (documents_entry_changed) {
        // 1. Let oldURL be document's latest entry's URL.
        auto old_url = m_latest_entry ? m_latest_entry->url() : URL::URL {};

        // 2. Set document's latest entry to entry.
        m_latest_entry = entry;

        // 3. Restore the history object state given document and entry.
        restore_the_history_object_state(entry);

        // 4. If documentIsNew is false, then:
        if (!document_is_new) {
            // NOTE: Not in the spec, but otherwise document's url won't be updated in case of a same-document back/forward navigation.
            set_url(entry->url());

            // 1. Assert: navigationType is not null.
            VERIFY(navigation_type.has_value());

            // AD HOC: Skip this in situations the spec steps don't account for
            if (update_navigation_api) {
                // 2. Update the navigation API entries for a same-document navigation given navigation, entry, and navigationType.
                navigation->update_the_navigation_api_entries_for_a_same_document_navigation(entry, navigation_type.value());
            }

            // 3. Fire an event named popstate at document's relevant global object, using PopStateEvent,
            //    with the state attribute initialized to document's history object's state and hasUAVisualTransition initialized to true
            //    if a visual transition, to display a cached rendered state of the latest entry, was done by the user agent.
            // FIXME: Initialise hasUAVisualTransition
            HTML::PopStateEventInit popstate_event_init;
            popstate_event_init.state = history()->unsafe_state();
            auto& relevant_global_object = as<HTML::Window>(HTML::relevant_global_object(*this));
            auto pop_state_event = HTML::PopStateEvent::create(realm(), "popstate"_fly_string, popstate_event_init);
            relevant_global_object.dispatch_event(pop_state_event);

            // FIXME: 4. Restore persisted state given entry.

            // 5. If oldURL's fragment is not equal to entry's URL's fragment, then queue a global task on the DOM manipulation task source
            //    given document's relevant global object to fire an event named hashchange at document's relevant global object,
            //    using HashChangeEvent, with the oldURL attribute initialized to the serialization of oldURL and the newURL attribute
            //    initialized to the serialization of entry's URL.
            if (old_url.fragment() != entry->url().fragment()) {
                HTML::HashChangeEventInit hashchange_event_init;
                hashchange_event_init.old_url = old_url.serialize();
                hashchange_event_init.new_url = entry->url().serialize();
                auto hashchange_event = HTML::HashChangeEvent::create(realm(), "hashchange"_fly_string, hashchange_event_init);
                HTML::queue_global_task(HTML::Task::Source::DOMManipulation, relevant_global_object, GC::create_function(heap(), [hashchange_event, &relevant_global_object]() {
                    relevant_global_object.dispatch_event(hashchange_event);
                }));
            }
        }

        // 5. Otherwise:
        else {
            // 1. Assert: entriesForNavigationAPI is given.
            VERIFY(entries_for_navigation_api.has_value());

            // FIXME: 2. Restore persisted state given entry.

            // 3. Initialize the navigation API entries for a new document given navigation, entriesForNavigationAPI, and entry.
            navigation->initialize_the_navigation_api_entries_for_a_new_document(*entries_for_navigation_api, entry);
        }
    }

    // FIXME: 7. If all the following are true:
    //    - previousEntryForActivation is given;
    //    - navigationType is non-null; and
    //    - navigationType is "reload" or previousEntryForActivation's document is not document, then:
    {
        // FIXME: 1. If navigation's activation is null, then set navigation's activation to a new NavigationActivation object in navigation's relevant realm.
        // FIXME: 2. Let previousEntryIndex be the result of getting the navigation API entry index of previousEntryForActivation within navigation.
        // FIXME: 3. If previousEntryIndex is non-negative, then set activation's old entry to navigation's entry list[previousEntryIndex].

        // FIXME: 4. Otherwise, if all the following are true:
        //    - navigationType is "replace";
        //    - previousEntryForActivation's document state's origin is same origin with document's origin; and
        //    - previousEntryForActivation's document's initial about:blank is false,
        //    then set activation's old entry to a new NavigationHistoryEntry in navigation's relevant realm, whose session history entry is previousEntryForActivation.

        // FIXME: 5. Set activation's new entry to navigation's current entry.
        // FIXME: 6. Set activation's navigation type to navigationType.
    }

    // 8. If documentIsNew is true, then:
    if (document_is_new) {
        // FIXME: 1. Assert: document's during-loading navigation ID for WebDriver BiDi is not null.
        // FIXME: 2. Invoke WebDriver BiDi navigation committed with navigable and a new WebDriver BiDi navigation
        //           status whose id is document's during-loading navigation ID for WebDriver BiDi, status is "committed", and url is document's URL

        // 3. Try to scroll to the fragment for document.
        try_to_scroll_to_the_fragment();

        // 4. At this point scripts may run for the newly-created document document.
        m_ready_to_run_scripts = true;
    }

    // 9. Otherwise, if documentsEntryChanged is false and doNotReactivate is false, then:
    // NOTE: This is for bfcache restoration
    if (!documents_entry_changed && !do_not_reactivate) {
        // FIXME: 1. Assert: entriesForNavigationAPI is given.
        // FIXME: 2. Reactivate document given entry and entriesForNavigationAPI.
    }
}

HashMap<URL::URL, GC::Ptr<HTML::SharedResourceRequest>>& Document::shared_resource_requests()
{
    return m_shared_resource_requests;
}

// https://www.w3.org/TR/web-animations-1/#dom-document-timeline
GC::Ref<Animations::DocumentTimeline> Document::timeline()
{
    // The DocumentTimeline object representing the default document timeline. The default document timeline has an
    // origin time of zero.
    if (!m_default_timeline)
        m_default_timeline = Animations::DocumentTimeline::create(realm(), *this, 0.0);
    return *m_default_timeline;
}

void Document::associate_with_timeline(GC::Ref<Animations::AnimationTimeline> timeline)
{
    m_associated_animation_timelines.set(timeline);
}

void Document::disassociate_with_timeline(GC::Ref<Animations::AnimationTimeline> timeline)
{
    m_associated_animation_timelines.remove(timeline);
}

void Document::append_pending_animation_event(Web::DOM::Document::PendingAnimationEvent const& event)
{
    m_pending_animation_event_queue.append(event);
}

// https://www.w3.org/TR/web-animations-1/#update-animations-and-send-events
void Document::update_animations_and_send_events(Optional<double> const& timestamp)
{
    // 1. Update the current time of all timelines associated with doc passing now as the timestamp.
    //
    // Note: Due to the hierarchical nature of the timing model, updating the current time of a timeline also involves:
    // - Updating the current time of any animations associated with the timeline.
    // - Running the update an animation’s finished state procedure for any animations whose current time has been
    //   updated.
    // - Queueing animation events for any such animations.
    m_last_animation_frame_timestamp = timestamp;
    for (auto const& timeline : m_associated_animation_timelines)
        timeline->set_current_time(timestamp);

    // 2. Remove replaced animations for doc.
    remove_replaced_animations();

    // 3. Perform a microtask checkpoint.
    HTML::perform_a_microtask_checkpoint();

    // 4. Let events to dispatch be a copy of doc’s pending animation event queue.
    auto events_to_dispatch = GC::ConservativeVector<Document::PendingAnimationEvent> { vm().heap() };
    events_to_dispatch.extend(m_pending_animation_event_queue);

    // 5. Clear doc’s pending animation event queue.
    m_pending_animation_event_queue.clear();

    // 6. Perform a stable sort of the animation events in events to dispatch as follows:
    auto sort_events_by_composite_order = [](auto const& a, auto const& b) {
        if (!a.animation->effect())
            return true;
        if (!b.animation->effect())
            return false;
        auto& a_effect = as<Animations::KeyframeEffect>(*a.animation->effect());
        auto& b_effect = as<Animations::KeyframeEffect>(*b.animation->effect());
        return Animations::KeyframeEffect::composite_order(a_effect, b_effect) < 0;
    };

    insertion_sort(events_to_dispatch, [&](auto const& a, auto const& b) {
        // Sort the events by their scheduled event time such that events that were scheduled to occur earlier, sort
        // before events scheduled to occur later and events whose scheduled event time is unresolved sort before events
        // with a resolved scheduled event time.
        //
        // Within events with equal scheduled event times, sort by their composite order.
        if (b.scheduled_event_time.has_value()) {
            if (!a.scheduled_event_time.has_value())
                return true;

            auto a_time = a.scheduled_event_time.value();
            auto b_time = b.scheduled_event_time.value();
            if (a_time == b_time)
                return sort_events_by_composite_order(a, b);

            return a.scheduled_event_time.value() < b.scheduled_event_time.value();
        }

        if (a.scheduled_event_time.has_value())
            return false;

        return sort_events_by_composite_order(a, b);
    });

    // 7. Dispatch each of the events in events to dispatch at their corresponding target using the order established in
    //    the previous step.
    for (auto const& event : events_to_dispatch)
        event.target->dispatch_event(event.event);

    for (auto& timeline : m_associated_animation_timelines) {
        for (auto& animation : timeline->associated_animations())
            dispatch_events_for_animation_if_necessary(animation);
    }
}

// https://www.w3.org/TR/web-animations-1/#remove-replaced-animations
void Document::remove_replaced_animations()
{
    // When asked to remove replaced animations for a Document, doc, then for every animation, animation, that:
    // - has an associated animation effect whose effect target is a descendant of doc, and
    // - is replaceable, and
    // - has a replace state of active, and
    // - for which there exists for each target property of every animation effect associated with animation, an
    //   animation effect associated with a replaceable animation with a higher composite order than animation that
    //   includes the same target property

    Vector<GC::Ref<Animations::Animation>> replaceable_animations;
    for (auto const& timeline : m_associated_animation_timelines) {
        for (auto const& animation : timeline->associated_animations()) {
            if (!animation->effect() || !animation->effect()->target() || &animation->effect()->target()->document() != this)
                continue;

            if (!animation->is_replaceable())
                continue;

            if (animation->replace_state() != Bindings::AnimationReplaceState::Active)
                continue;

            // Composite order is only defined for KeyframeEffects
            if (!animation->effect()->is_keyframe_effect())
                continue;

            replaceable_animations.append(animation);
        }
    }

    quick_sort(replaceable_animations, [](GC::Ref<Animations::Animation>& a, GC::Ref<Animations::Animation>& b) {
        VERIFY(a->effect()->is_keyframe_effect());
        VERIFY(b->effect()->is_keyframe_effect());
        auto& a_effect = *static_cast<Animations::KeyframeEffect*>(a->effect().ptr());
        auto& b_effect = *static_cast<Animations::KeyframeEffect*>(b->effect().ptr());
        return Animations::KeyframeEffect::composite_order(a_effect, b_effect) < 0;
    });

    // Lower value = higher priority
    HashMap<CSS::PropertyID, size_t> highest_property_composite_orders;
    for (int i = replaceable_animations.size() - 1; i >= 0; i--) {
        auto animation = replaceable_animations[i];
        bool has_any_highest_priority_property = false;

        for (auto const& property : animation->effect()->target_properties()) {
            if (!highest_property_composite_orders.contains(property)) {
                has_any_highest_priority_property = true;
                highest_property_composite_orders.set(property, i);
            }
        }

        if (!has_any_highest_priority_property) {
            // perform the following steps:

            // - Set animation’s replace state to removed.
            animation->set_replace_state(Bindings::AnimationReplaceState::Removed);

            // - Create an AnimationPlaybackEvent, removeEvent.
            // - Set removeEvent’s type attribute to remove.
            // - Set removeEvent’s currentTime attribute to the current time of animation.
            // - Set removeEvent’s timelineTime attribute to the current time of the timeline with which animation is
            //   associated.
            Animations::AnimationPlaybackEventInit init;
            init.current_time = animation->current_time();
            init.timeline_time = animation->timeline()->current_time();
            auto remove_event = Animations::AnimationPlaybackEvent::create(realm(), HTML::EventNames::remove, init);

            // - If animation has a document for timing, then append removeEvent to its document for timing's pending
            //   animation event queue along with its target, animation. For the scheduled event time, use the result of
            //   applying the procedure to convert timeline time to origin-relative time to the current time of the
            //   timeline with which animation is associated.
            if (auto document = animation->document_for_timing()) {
                PendingAnimationEvent pending_animation_event {
                    .event = remove_event,
                    .animation = animation,
                    .target = animation,
                    .scheduled_event_time = animation->timeline()->convert_a_timeline_time_to_an_origin_relative_time(init.timeline_time),
                };
                document->append_pending_animation_event(pending_animation_event);
            }
            //   Otherwise, queue a task to dispatch removeEvent at animation. The task source for this task is the DOM
            //   manipulation task source.
            else {
                HTML::queue_global_task(HTML::Task::Source::DOMManipulation, realm().global_object(), GC::create_function(heap(), [animation, remove_event]() {
                    animation->dispatch_event(remove_event);
                }));
            }
        }
    }
}

WebIDL::ExceptionOr<Vector<GC::Ref<Animations::Animation>>> Document::get_animations()
{
    Vector<GC::Ref<Animations::Animation>> relevant_animations;
    TRY(for_each_child_of_type_fallible<Element>([&](auto& child) -> WebIDL::ExceptionOr<IterationDecision> {
        relevant_animations.extend(TRY(child.get_animations(Animations::GetAnimationsOptions { .subtree = true })));
        return IterationDecision::Continue;
    }));
    return relevant_animations;
}

// https://html.spec.whatwg.org/multipage/dom.html#dom-document-nameditem-filter
static bool is_potentially_named_element(DOM::Element const& element)
{
    return is<HTML::HTMLEmbedElement>(element) || is<HTML::HTMLFormElement>(element) || is<HTML::HTMLIFrameElement>(element) || is<HTML::HTMLImageElement>(element) || is<HTML::HTMLObjectElement>(element);
}

// https://html.spec.whatwg.org/multipage/dom.html#dom-document-nameditem-filter
static bool is_potentially_named_element_by_id(DOM::Element const& element)
{
    return is<HTML::HTMLObjectElement>(element) || is<HTML::HTMLImageElement>(element);
}

static void insert_in_tree_order(Vector<GC::Ref<DOM::Element>>& elements, DOM::Element& element)
{
    for (auto& el : elements) {
        if (el == &element)
            return;
    }

    auto index = elements.find_first_index_if([&](auto& existing_element) {
        return existing_element->compare_document_position(element) & Node::DOCUMENT_POSITION_FOLLOWING;
    });
    if (index.has_value())
        elements.insert(index.value(), element);
    else
        elements.append(element);
}

void Document::element_id_changed(Badge<DOM::Element>, GC::Ref<DOM::Element> element, Optional<FlyString> old_id)
{
    for (auto* form_associated_element : m_form_associated_elements_with_form_attribute)
        form_associated_element->element_id_changed({});

    if (element->id().has_value())
        insert_in_tree_order(m_potentially_named_elements, element);
    else
        (void)m_potentially_named_elements.remove_first_matching([element](auto& e) { return e == element; });

    auto new_id = element->id();
    if (old_id.has_value()) {
        element->document_or_shadow_root_element_by_id_map().remove(old_id.value(), element);
    }
    if (new_id.has_value()) {
        element->document_or_shadow_root_element_by_id_map().add(new_id.value(), element);
    }
}

void Document::element_with_id_was_added(Badge<DOM::Element>, GC::Ref<DOM::Element> element)
{
    for (auto* form_associated_element : m_form_associated_elements_with_form_attribute)
        form_associated_element->element_with_id_was_added_or_removed({});

    if (is_potentially_named_element_by_id(*element))
        insert_in_tree_order(m_potentially_named_elements, element);

    if (auto id = element->id(); id.has_value()) {
        element->document_or_shadow_root_element_by_id_map().add(id.value(), element);
    }
}

void Document::element_with_id_was_removed(Badge<DOM::Element>, GC::Ref<DOM::Element> element)
{
    for (auto* form_associated_element : m_form_associated_elements_with_form_attribute)
        form_associated_element->element_with_id_was_added_or_removed({});

    if (is_potentially_named_element_by_id(*element))
        (void)m_potentially_named_elements.remove_first_matching([element](auto& e) { return e == element; });

    if (auto id = element->id(); id.has_value()) {
        element->document_or_shadow_root_element_by_id_map().remove(id.value(), element);
    }
}

void Document::element_name_changed(Badge<DOM::Element>, GC::Ref<DOM::Element> element)
{
    if (element->name().has_value()) {
        insert_in_tree_order(m_potentially_named_elements, element);
    } else {
        if (is_potentially_named_element_by_id(element) && element->id().has_value())
            return;
        (void)m_potentially_named_elements.remove_first_matching([element](auto& e) { return e == element; });
    }
}

void Document::element_with_name_was_added(Badge<DOM::Element>, GC::Ref<DOM::Element> element)
{
    if (is_potentially_named_element(element))
        insert_in_tree_order(m_potentially_named_elements, element);
}

void Document::element_with_name_was_removed(Badge<DOM::Element>, GC::Ref<DOM::Element> element)
{
    if (is_potentially_named_element(element)) {
        if (is_potentially_named_element_by_id(element) && element->id().has_value())
            return;
        (void)m_potentially_named_elements.remove_first_matching([element](auto& e) { return e == element; });
    }
}

void Document::add_form_associated_element_with_form_attribute(HTML::FormAssociatedElement& form_associated_element)
{
    m_form_associated_elements_with_form_attribute.append(&form_associated_element);
}

void Document::remove_form_associated_element_with_form_attribute(HTML::FormAssociatedElement& form_associated_element)
{
    m_form_associated_elements_with_form_attribute.remove_all_matching([&](auto* element) {
        return element == &form_associated_element;
    });
}

void Document::set_design_mode_enabled_state(bool design_mode_enabled)
{
    m_design_mode_enabled = design_mode_enabled;
    set_editable(design_mode_enabled);
}

// https://html.spec.whatwg.org/multipage/interaction.html#making-entire-documents-editable:-the-designmode-idl-attribute
String Document::design_mode() const
{
    // The designMode getter steps are to return "on" if this's design mode enabled is true; otherwise "off".
    return design_mode_enabled_state() ? "on"_string : "off"_string;
}

WebIDL::ExceptionOr<void> Document::set_design_mode(String const& design_mode)
{
    // 1. Let value be the given value, converted to ASCII lowercase.
    auto value = MUST(design_mode.to_lowercase());

    // 2. If value is "on" and this's design mode enabled is false, then:
    if (value == "on"sv && !m_design_mode_enabled) {
        // 1. Set this's design mode enabled to true.
        set_design_mode_enabled_state(true);
        // 2. Reset this's active range's start and end boundary points to be at the start of this.
        if (auto selection = get_selection(); selection) {
            if (auto active_range = selection->range(); active_range) {
                TRY(active_range->set_start(*this, 0));
                TRY(active_range->set_end(*this, 0));
                update_layout(UpdateLayoutReason::DocumentSetDesignMode);
            }
        }
        // 3. Run the focusing steps for this's document element, if non-null.
        if (auto* document_element = this->document_element(); document_element)
            HTML::run_focusing_steps(document_element);
    }
    // 3. If value is "off", then set this's design mode enabled to false.
    else if (value == "off"sv) {
        set_design_mode_enabled_state(false);
    }
    return {};
}

// https://drafts.csswg.org/cssom-view/#dom-document-elementfrompoint
Element const* Document::element_from_point(double x, double y)
{
    // 1. If either argument is negative, x is greater than the viewport width excluding the size of a rendered scroll
    //    bar (if any), or y is greater than the viewport height excluding the size of a rendered scroll bar (if any), or
    //    there is no viewport associated with the document, return null and terminate these steps.
    auto viewport_rect = this->viewport_rect();
    CSSPixelPoint position { x, y };
    // FIXME: This should account for the size of the scroll bar.
    if (x < 0 || y < 0 || position.x() > viewport_rect.width() || position.y() > viewport_rect.height())
        return nullptr;

    // Ensure the layout tree exists prior to hit testing.
    update_layout(UpdateLayoutReason::DocumentElementFromPoint);

    // 2. If there is a box in the viewport that would be a target for hit testing at coordinates x,y, when applying the transforms
    //    that apply to the descendants of the viewport, return the associated element and terminate these steps.
    Optional<Painting::HitTestResult> hit_test_result;
    if (auto const* paintable_box = this->paintable_box(); paintable_box) {
        (void)paintable_box->hit_test(position, Painting::HitTestType::Exact, [&](Painting::HitTestResult result) {
            auto* dom_node = result.dom_node();
            if (dom_node && dom_node->is_element()) {
                hit_test_result = result;
                return TraversalDecision::Break;
            }
            return TraversalDecision::Continue;
        });
    }
    if (hit_test_result.has_value())
        return static_cast<Element*>(hit_test_result->dom_node());

    // 3. If the document has a root element, return the root element and terminate these steps.
    if (auto const* document_root_element = first_child_of_type<Element>(); document_root_element)
        return document_root_element;

    // 4. Return null.
    return nullptr;
}

// https://drafts.csswg.org/cssom-view/#dom-document-elementsfrompoint
GC::RootVector<GC::Ref<Element>> Document::elements_from_point(double x, double y)
{
    // 1. Let sequence be a new empty sequence.
    GC::RootVector<GC::Ref<Element>> sequence(heap());

    // 2. If either argument is negative, x is greater than the viewport width excluding the size of a rendered scroll bar (if any),
    //    or y is greater than the viewport height excluding the size of a rendered scroll bar (if any),
    //    or there is no viewport associated with the document, return sequence and terminate these steps.
    auto viewport_rect = this->viewport_rect();
    CSSPixelPoint position { x, y };
    // FIXME: This should account for the size of the scroll bar.
    if (x < 0 || y < 0 || position.x() > viewport_rect.width() || position.y() > viewport_rect.height())
        return sequence;

    // Ensure the layout tree exists prior to hit testing.
    update_layout(UpdateLayoutReason::DocumentElementsFromPoint);

    // 3. For each box in the viewport, in paint order, starting with the topmost box, that would be a target for
    //    hit testing at coordinates x,y even if nothing would be overlapping it, when applying the transforms that
    //    apply to the descendants of the viewport, append the associated element to sequence.
    if (auto const* paintable_box = this->paintable_box(); paintable_box) {
        (void)paintable_box->hit_test(position, Painting::HitTestType::Exact, [&](Painting::HitTestResult result) {
            auto* dom_node = result.dom_node();
            if (dom_node && dom_node->is_element() && result.paintable->visible_for_hit_testing())
                sequence.append(*static_cast<Element*>(dom_node));
            return TraversalDecision::Continue;
        });
    }

    // 4. If the document has a root element, and the last item in sequence is not the root element,
    //    append the root element to sequence.
    if (auto* root_element = document_element(); root_element && (sequence.is_empty() || (sequence.last() != root_element)))
        sequence.append(*root_element);

    // 5. Return sequence.
    return sequence;
}

// https://drafts.csswg.org/cssom-view/#dom-document-scrollingelement
GC::Ptr<Element const> Document::scrolling_element() const
{
    // 1. If the Document is in quirks mode, follow these substeps:
    if (in_quirks_mode()) {
        // 1. If the body element exists, and it is not potentially scrollable, return the body element and abort these steps.
        //    For this purpose, a value of overflow:clip on the the body element’s parent element must be treated as overflow:hidden.
        if (auto const* body_element = body(); body_element && !body_element->is_potentially_scrollable())
            return body_element;

        // 2. Return null and abort these steps.
        return nullptr;
    }

    // 2. If there is a root element, return the root element and abort these steps.
    if (auto const* root_element = document_element(); root_element)
        return root_element;

    // 3. Return null.
    return nullptr;
}

// https://html.spec.whatwg.org/multipage/dom.html#exposed
static bool is_exposed(Element const& element)
{
    VERIFY(is<HTML::HTMLEmbedElement>(element) || is<HTML::HTMLObjectElement>(element));

    // FIXME: An embed or object element is said to be exposed if it has no exposed object ancestor, and,
    //        for object elements, is additionally either not showing its fallback content or has no object or embed descendants.
    return true;
}

// https://html.spec.whatwg.org/multipage/dom.html#dom-tree-accessors:supported-property-names
Vector<FlyString> Document::supported_property_names() const
{
    // The supported property names of a Document object document at any moment consist of the following,
    // in tree order according to the element that contributed them, ignoring later duplicates,
    // and with values from id attributes coming before values from name attributes when the same element contributes both:
    OrderedHashTable<FlyString> names;

    for (auto const& element : m_potentially_named_elements) {
        // - the value of the name content attribute for all exposed embed, form, iframe, img, and exposed object elements
        //   that have a non-empty name content attribute and are in a document tree with document as their root;
        if ((is<HTML::HTMLEmbedElement>(*element) && is_exposed(element))
            || is<HTML::HTMLFormElement>(*element)
            || is<HTML::HTMLIFrameElement>(*element)
            || is<HTML::HTMLImageElement>(*element)
            || (is<HTML::HTMLObjectElement>(*element) && is_exposed(element))) {
            if (auto name = element->name(); name.has_value()) {
                names.set(name.value());
            }
        }

        // - the value of the id content attribute for all exposed object elements that have a non-empty id content attribute
        //   and are in a document tree with document as their root; and
        if (is<HTML::HTMLObjectElement>(*element) && is_exposed(element)) {
            if (auto id = element->id(); id.has_value()) {
                names.set(id.value());
            }
        }

        // - the value of the id content attribute for all img elements that have both a non-empty id content attribute
        //   and a non-empty name content attribute, and are in a document tree with document as their root.
        if (is<HTML::HTMLImageElement>(*element)) {
            if (auto id = element->id(); id.has_value() && element->name().has_value()) {
                names.set(id.value());
            }
        }
    }

    return names.values();
}

static bool is_named_element_with_name(Element const& element, FlyString const& name)
{
    // Named elements with the name name, for the purposes of the above algorithm, are those that are either:

    // - Exposed embed, form, iframe, img, or exposed object elements that have a name content attribute whose value
    //   is name, or
    if ((is<HTML::HTMLEmbedElement>(element) && is_exposed(element))
        || is<HTML::HTMLFormElement>(element)
        || is<HTML::HTMLIFrameElement>(element)
        || is<HTML::HTMLImageElement>(element)
        || (is<HTML::HTMLObjectElement>(element) && is_exposed(element))) {
        if (element.name() == name)
            return true;
    }

    // - Exposed object elements that have an id content attribute whose value is name, or
    if (is<HTML::HTMLObjectElement>(element) && is_exposed(element)) {
        if (element.id() == name)
            return true;
    }

    // - img elements that have an id content attribute whose value is name, and that have a non-empty name content
    //   attribute present also.
    if (is<HTML::HTMLImageElement>(element)) {
        if (element.id() == name && element.name().has_value())
            return true;
    }

    return false;
}

static Vector<GC::Ref<DOM::Element>> named_elements_with_name(Document const& document, FlyString const& name)
{
    Vector<GC::Ref<DOM::Element>> named_elements;

    for (auto const& element : document.potentially_named_elements()) {
        if (is_named_element_with_name(element, name))
            named_elements.append(element);
    }

    return named_elements;
}

// https://html.spec.whatwg.org/multipage/dom.html#dom-document-nameditem
JS::Value Document::named_item_value(FlyString const& name) const
{
    // 1. Let elements be the list of named elements with the name name that are in a document tree with the Document as their root.
    // NOTE: There will be at least one such element, since the algorithm would otherwise not have been invoked by Web IDL.
    auto elements = named_elements_with_name(*this, name);

    // 2. If elements has only one element, and that element is an iframe element, and that iframe element's content navigable is not null,
    //    then return the active WindowProxy of the element's content navigable.
    if (elements.size() == 1 && is<HTML::HTMLIFrameElement>(*elements.first())) {
        auto& iframe_element = static_cast<HTML::HTMLIFrameElement&>(*elements.first());
        if (iframe_element.content_navigable() != nullptr)
            return iframe_element.content_navigable()->active_window_proxy();
    }

    // 3. Otherwise, if elements has only one element, return that element.
    if (elements.size() == 1)
        return elements.first();

    // 4. Otherwise return an HTMLCollection rooted at the Document node, whose filter matches only named elements with the name name.
    return HTMLCollection::create(*const_cast<Document*>(this), HTMLCollection::Scope::Descendants, [name](auto& element) {
        return is_named_element_with_name(element, name);
    });
}

// https://drafts.csswg.org/resize-observer-1/#calculate-depth-for-node
static size_t calculate_depth_for_node(Node const& node)
{
    // 1. Let p be the parent-traversal path from node to a root Element of this element’s flattened DOM tree.
    // 2. Return number of nodes in p.

    size_t depth = 0;
    for (auto const* current = &node; current; current = current->parent())
        ++depth;
    return depth;
}

// https://drafts.csswg.org/resize-observer-1/#gather-active-observations-h
void Document::gather_active_observations_at_depth(size_t depth)
{
    // 1. Let depth be the depth passed in.

    // 2. For each observer in [[resizeObservers]] run these steps:
    for (auto const& observer : m_resize_observers) {
        // 1. Clear observer’s [[activeTargets]], and [[skippedTargets]].
        observer->active_targets().clear();
        observer->skipped_targets().clear();

        // 2. For each observation in observer.[[observationTargets]] run this step:
        for (auto const& observation : observer->observation_targets()) {
            // 1. If observation.isActive() is true
            if (observation->is_active()) {
                // 1. Let targetDepth be result of calculate depth for node for observation.target.
                auto target_depth = calculate_depth_for_node(*observation->target());

                // 2. If targetDepth is greater than depth then add observation to [[activeTargets]].
                if (target_depth > depth) {
                    observer->active_targets().append(observation);
                } else {
                    // 3. Else add observation to [[skippedTargets]].
                    observer->skipped_targets().append(observation);
                }
            }
        }
    }
}

// https://drafts.csswg.org/resize-observer-1/#broadcast-active-resize-observations
size_t Document::broadcast_active_resize_observations()
{
    // 1. Let shallowestTargetDepth be ∞
    auto shallowest_target_depth = NumericLimits<size_t>::max();

    // 2. For each observer in document.[[resizeObservers]] run these steps:

    // NOTE: We make a copy of the resize observers list to avoid modifying it while iterating.
    GC::RootVector<GC::Ref<ResizeObserver::ResizeObserver>> resize_observers(heap());
    resize_observers.ensure_capacity(m_resize_observers.size());
    for (auto const& observer : m_resize_observers)
        resize_observers.append(observer);

    for (auto const& observer : resize_observers) {
        // 1. If observer.[[activeTargets]] slot is empty, continue.
        if (observer->active_targets().is_empty()) {
            continue;
        }

        // 2. Let entries be an empty list of ResizeObserverEntryies.
        GC::RootVector<GC::Ref<ResizeObserver::ResizeObserverEntry>> entries(heap());

        // 3. For each observation in [[activeTargets]] perform these steps:
        for (auto const& observation : observer->active_targets()) {
            // 1. Let entry be the result of running create and populate a ResizeObserverEntry given observation.target.
            auto entry = ResizeObserver::ResizeObserverEntry::create_and_populate(realm(), *observation->target()).release_value_but_fixme_should_propagate_errors();

            // 2. Add entry to entries.
            entries.append(entry);

            // 3. Set observation.lastReportedSizes to matching entry sizes.
            switch (observation->observed_box()) {
            case Bindings::ResizeObserverBoxOptions::BorderBox:
                // Matching sizes are entry.borderBoxSize if observation.observedBox is "border-box"
                observation->last_reported_sizes() = entry->border_box_size();
                break;
            case Bindings::ResizeObserverBoxOptions::ContentBox:
                // Matching sizes are entry.contentBoxSize if observation.observedBox is "content-box"
                observation->last_reported_sizes() = entry->content_box_size();
                break;
            case Bindings::ResizeObserverBoxOptions::DevicePixelContentBox:
                // Matching sizes are entry.devicePixelContentBoxSize if observation.observedBox is "device-pixel-content-box"
                observation->last_reported_sizes() = entry->device_pixel_content_box_size();
                break;
            default:
                VERIFY_NOT_REACHED();
            }

            // 4. Set targetDepth to the result of calculate depth for node for observation.target.
            auto target_depth = calculate_depth_for_node(*observation->target());

            // 5. Set shallowestTargetDepth to targetDepth if targetDepth < shallowestTargetDepth
            if (target_depth < shallowest_target_depth)
                shallowest_target_depth = target_depth;
        }

        // 4. Invoke observer.[[callback]] with entries.
        observer->invoke_callback(entries);

        // 5. Clear observer.[[activeTargets]].
        observer->active_targets().clear();
    }

    return shallowest_target_depth;
}

// https://drafts.csswg.org/resize-observer-1/#has-active-observations-h
bool Document::has_active_resize_observations()
{
    // 1. For each observer in [[resizeObservers]] run this step:
    for (auto const& observer : m_resize_observers) {
        // 1. If observer.[[activeTargets]] is not empty, return true.
        if (!observer->active_targets().is_empty())
            return true;
    }

    // 2. Return false.
    return false;
}

// https://drafts.csswg.org/resize-observer-1/#has-skipped-observations-h
bool Document::has_skipped_resize_observations()
{
    // 1. For each observer in [[resizeObservers]] run this step:
    for (auto const& observer : m_resize_observers) {
        // 1. If observer.[[skippedTargets]] is not empty, return true.
        if (!observer->skipped_targets().is_empty())
            return true;
    }

    // 2. Return false.
    return false;
}

GC::Ref<WebIDL::ObservableArray> Document::adopted_style_sheets() const
{
    if (!m_adopted_style_sheets)
        m_adopted_style_sheets = create_adopted_style_sheets_list(const_cast<Document&>(*this));
    return *m_adopted_style_sheets;
}

WebIDL::ExceptionOr<void> Document::set_adopted_style_sheets(JS::Value new_value)
{
    if (!m_adopted_style_sheets)
        m_adopted_style_sheets = create_adopted_style_sheets_list(const_cast<Document&>(*this));

    m_adopted_style_sheets->clear();
    auto iterator_record = TRY(get_iterator(vm(), new_value, JS::IteratorHint::Sync));
    while (true) {
        auto next = TRY(iterator_step_value(vm(), iterator_record));
        if (!next.has_value())
            break;
        TRY(m_adopted_style_sheets->append(*next));
    }

    return {};
}

void Document::for_each_active_css_style_sheet(Function<void(CSS::CSSStyleSheet&, GC::Ptr<DOM::ShadowRoot>)>&& callback) const
{
    if (m_style_sheets) {
        for (auto& style_sheet : m_style_sheets->sheets()) {
            if (!(style_sheet->is_alternate() && style_sheet->disabled()))
                callback(*style_sheet, {});
        }
    }

    if (m_adopted_style_sheets) {
        m_adopted_style_sheets->for_each<CSS::CSSStyleSheet>([&](auto& style_sheet) {
            if (!style_sheet.disabled())
                callback(style_sheet, {});
        });
    }

    for_each_shadow_root([&](auto& shadow_root) {
        shadow_root.for_each_css_style_sheet([&](auto& style_sheet) {
            if (!style_sheet.disabled())
                callback(style_sheet, &shadow_root);
        });
    });
}

static Optional<CSS::CSSStyleSheet&> find_style_sheet_with_url(String const& url, CSS::CSSStyleSheet& style_sheet)
{
    if (style_sheet.location() == url)
        return style_sheet;

    for (auto& import_rule : style_sheet.import_rules()) {
        if (import_rule->loaded_style_sheet()) {
            if (auto match = find_style_sheet_with_url(url, *import_rule->loaded_style_sheet()); match.has_value())
                return match;
        }
    }

    return {};
}

Optional<String> Document::get_style_sheet_source(CSS::StyleSheetIdentifier const& identifier) const
{
    switch (identifier.type) {
    case CSS::StyleSheetIdentifier::Type::StyleElement:
        if (identifier.dom_element_unique_id.has_value()) {
            if (auto* node = Node::from_unique_id(*identifier.dom_element_unique_id)) {
                if (node->is_html_style_element()) {
                    if (auto* sheet = as<HTML::HTMLStyleElement>(*node).sheet())
                        return sheet->source_text({});
                }
                if (node->is_svg_style_element()) {
                    if (auto* sheet = as<SVG::SVGStyleElement>(*node).sheet())
                        return sheet->source_text({});
                }
            }
        }
        return {};
    case CSS::StyleSheetIdentifier::Type::LinkElement:
    case CSS::StyleSheetIdentifier::Type::ImportRule: {
        if (!identifier.url.has_value()) {
            dbgln("Attempting to get link or imported style-sheet with no url; giving up");
            return {};
        }

        if (m_style_sheets) {
            for (auto& style_sheet : m_style_sheets->sheets()) {
                if (auto match = find_style_sheet_with_url(identifier.url.value(), style_sheet); match.has_value())
                    return match->source_text({});
            }
        }

        if (m_adopted_style_sheets) {
            Optional<String> result;
            m_adopted_style_sheets->for_each<CSS::CSSStyleSheet>([&](auto& style_sheet) {
                if (result.has_value())
                    return;

                if (auto match = find_style_sheet_with_url(identifier.url.value(), style_sheet); match.has_value())
                    result = match->source_text({});
            });
            return result;
        }

        return {};
    }
    case CSS::StyleSheetIdentifier::Type::UserAgent:
        return CSS::StyleComputer::user_agent_style_sheet_source(identifier.url.value());
    case CSS::StyleSheetIdentifier::Type::UserStyle:
        return page().user_style();
    }

    return {};
}

void Document::register_shadow_root(Badge<DOM::ShadowRoot>, DOM::ShadowRoot& shadow_root)
{
    m_shadow_roots.append(shadow_root);
}

void Document::unregister_shadow_root(Badge<DOM::ShadowRoot>, DOM::ShadowRoot& shadow_root)
{
    m_shadow_roots.remove_all_matching([&](auto& item) {
        return item.ptr() == &shadow_root;
    });
}

void Document::for_each_shadow_root(Function<void(DOM::ShadowRoot&)>&& callback)
{
    for (auto& shadow_root : m_shadow_roots)
        callback(shadow_root);
}

void Document::for_each_shadow_root(Function<void(DOM::ShadowRoot&)>&& callback) const
{
    for (auto& shadow_root : m_shadow_roots)
        callback(shadow_root);
}

bool Document::is_decoded_svg() const
{
    return is<Web::SVG::SVGDecodedImageData::SVGPageClient>(page().client());
}

// https://drafts.csswg.org/css-position-4/#add-an-element-to-the-top-layer
void Document::add_an_element_to_the_top_layer(GC::Ref<Element> element)
{
    // 1. Let doc be el’s node document.

    // 2. If el is already contained in doc’s top layer:
    if (m_top_layer_elements.contains(element)) {
        // Assert: el is also in doc’s pending top layer removals. (Otherwise, this is a spec error.)
        VERIFY(m_top_layer_pending_removals.contains(element));

        // Remove el from both doc’s top layer and pending top layer removals.
        m_top_layer_elements.remove(element);
        m_top_layer_pending_removals.remove(element);
    }

    // 3. Append el to doc’s top layer.
    m_top_layer_elements.set(element);
    element->set_in_top_layer(true);

    // FIXME: 4. At the UA !important cascade origin, add a rule targeting el containing an overlay: auto declaration.
    element->set_rendered_in_top_layer(true);
    element->set_needs_style_update(true);
    invalidate_layout_tree(InvalidateLayoutTreeReason::DocumentAddAnElementToTheTopLayer);
}

// https://drafts.csswg.org/css-position-4/#request-an-element-to-be-removed-from-the-top-layer
void Document::request_an_element_to_be_remove_from_the_top_layer(GC::Ref<Element> element)
{
    // 1. Let doc be el’s node document.

    // 2. If el is not contained doc’s top layer, or el is already contained in doc’s pending top layer removals, return.
    if (!m_top_layer_elements.contains(element) || m_top_layer_pending_removals.contains(element))
        return;

    // FIXME: 3. Remove the UA !important overlay: auto rule targeting el.
    element->set_rendered_in_top_layer(false);
    element->set_needs_style_update(true);
    invalidate_layout_tree(InvalidateLayoutTreeReason::DocumentRequestAnElementToBeRemovedFromTheTopLayer);

    // 4. Append el to doc’s pending top layer removals.
    m_top_layer_pending_removals.set(element);
    element->set_in_top_layer(false);
}

// https://drafts.csswg.org/css-position-4/#remove-an-element-from-the-top-layer-immediately
void Document::remove_an_element_from_the_top_layer_immediately(GC::Ref<Element> element)
{
    // 1. Let doc be el’s node document.

    // 2. Remove el from doc’s top layer and pending top layer removals.
    m_top_layer_elements.remove(element);
    element->set_in_top_layer(false);

    // FIXME: 3. Remove the UA !important overlay: auto rule targeting el, if it exists.
    element->set_rendered_in_top_layer(false);
    element->set_needs_style_update(true);
}

// https://drafts.csswg.org/css-position-4/#process-top-layer-removals
void Document::process_top_layer_removals()
{
    // 1. For each element el in doc’s pending top layer removals: if el’s computed value of overlay is none, or el is
    //    not rendered, remove el from doc’s top layer and pending top layer removals.
    for (auto& element : m_top_layer_pending_removals) {
        // FIXME: Implement overlay property
        if (true || !element->paintable()) {
            m_top_layer_elements.remove(element);
            m_top_layer_pending_removals.remove(element);
        }
    }
}

void Document::set_needs_to_refresh_scroll_state(bool b)
{
    if (auto* paintable = this->paintable())
        paintable->set_needs_to_refresh_scroll_state(b);
}

Vector<GC::Root<DOM::Range>> Document::find_matching_text(String const& query, CaseSensitivity case_sensitivity)
{
    // Ensure the layout tree exists before searching for text matches.
    update_layout(UpdateLayoutReason::DocumentFindMatchingText);

    if (!layout_node())
        return {};

    auto const& text_blocks = layout_node()->text_blocks();
    if (text_blocks.is_empty())
        return {};

    Vector<GC::Root<DOM::Range>> matches;
    for (auto const& text_block : text_blocks) {
        size_t offset = 0;
        size_t i = 0;
        auto const& text = text_block.text;
        auto* match_start_position = &text_block.positions[0];
        while (true) {
            auto match_index = case_sensitivity == CaseSensitivity::CaseInsensitive
                ? text.find_byte_offset_ignoring_case(query, offset)
                : text.find_byte_offset(query, offset);
            if (!match_index.has_value())
                break;

            for (; i < text_block.positions.size() - 1 && match_index.value() > text_block.positions[i + 1].start_offset; ++i)
                match_start_position = &text_block.positions[i + 1];

            auto start_position = match_index.value() - match_start_position->start_offset;
            auto& start_dom_node = match_start_position->dom_node;

            auto* match_end_position = match_start_position;
            for (; i < text_block.positions.size() - 1 && (match_index.value() + query.bytes_as_string_view().length() > text_block.positions[i + 1].start_offset); ++i)
                match_end_position = &text_block.positions[i + 1];

            auto& end_dom_node = match_end_position->dom_node;
            auto end_position = match_index.value() + query.bytes_as_string_view().length() - match_end_position->start_offset;

            matches.append(Range::create(start_dom_node, start_position, end_dom_node, end_position));
            match_start_position = match_end_position;
            offset = match_index.value() + query.bytes_as_string_view().length() + 1;
            if (offset >= text.bytes_as_string_view().length())
                break;
        }
    }

    return matches;
}

// https://dom.spec.whatwg.org/#document-allow-declarative-shadow-roots
bool Document::allow_declarative_shadow_roots() const
{
    return m_allow_declarative_shadow_roots;
}

bool Document::is_render_blocking_element(GC::Ref<Element> element) const
{
    return m_render_blocking_elements.contains(element);
}

// https://html.spec.whatwg.org/multipage/dom.html#render-blocked
bool Document::is_render_blocked() const
{
    // A Document document is render-blocked if both of the following are true:
    // - document's render-blocking element set is non-empty, or document allows adding render-blocking elements.
    // - The current high resolution time given document's relevant global object has not exceeded an implementation-defined timeout value.

    // NOTE: This timeout is implementation-defined.
    //       Other browsers are willing to wait longer, but let's start with 30 seconds.
    static constexpr auto max_time_to_block_rendering_in_ms = 30000.0;

    auto now = HighResolutionTime::current_high_resolution_time(relevant_global_object(*this));
    if (now > max_time_to_block_rendering_in_ms)
        return false;

    return !m_render_blocking_elements.is_empty() || allows_adding_render_blocking_elements();
}

// https://html.spec.whatwg.org/multipage/dom.html#allows-adding-render-blocking-elements
bool Document::allows_adding_render_blocking_elements() const
{
    // A document allows adding render-blocking elements if document's content type is "text/html" and the body element of document is null.
    return content_type() == "text/html" && !body();
}

void Document::add_render_blocking_element(GC::Ref<Element> element)
{
    m_render_blocking_elements.set(element);
}

void Document::remove_render_blocking_element(GC::Ref<Element> element)
{
    m_render_blocking_elements.remove(element);
}

// https://dom.spec.whatwg.org/#document-allow-declarative-shadow-roots
void Document::set_allow_declarative_shadow_roots(bool allow)
{
    m_allow_declarative_shadow_roots = allow;
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#parse-html-from-a-string
void Document::parse_html_from_a_string(StringView html)
{
    // 1. Set document's type to "html".
    set_document_type(DOM::Document::Type::HTML);

    // 2. Create an HTML parser parser, associated with document.
    // 3. Place html into the input stream for parser. The encoding confidence is irrelevant.
    // FIXME: We don't have the concept of encoding confidence yet.
    auto parser = HTML::HTMLParser::create(*this, html, "UTF-8"sv);

    // 4. Start parser and let it run until it has consumed all the characters just inserted into the input stream.
    parser->run(as<HTML::Window>(HTML::relevant_global_object(*this)).associated_document().url());
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-parsehtmlunsafe
GC::Ref<Document> Document::parse_html_unsafe(JS::VM& vm, StringView html)
{
    auto& realm = *vm.current_realm();
    // FIXME: 1. Let compliantHTML to the result of invoking the Get Trusted Type compliant string algorithm with TrustedHTML, this's relevant global object, html, "Document parseHTMLUnsafe", and "script".

    // 2. Let document be a new Document, whose content type is "text/html".
    auto document = Document::create_for_fragment_parsing(realm);
    document->set_content_type("text/html"_string);

    // 3. Set document's allow declarative shadow roots to true.
    document->set_allow_declarative_shadow_roots(true);

    // 4. Parse HTML from a string given document and compliantHTML. // FIXME: Use compliantHTML.
    document->parse_html_from_a_string(html);

    // 5. Return document.
    return document;
}

InputEventsTarget* Document::active_input_events_target()
{
    auto* focused_element = this->focused_element();
    if (!focused_element)
        return {};

    if (is<HTML::HTMLInputElement>(*focused_element))
        return static_cast<HTML::HTMLInputElement*>(focused_element);
    if (is<HTML::HTMLTextAreaElement>(*focused_element))
        return static_cast<HTML::HTMLTextAreaElement*>(focused_element);
    if (focused_element->is_editable_or_editing_host())
        return m_editing_host_manager;
    return nullptr;
}

GC::Ptr<DOM::Position> Document::cursor_position() const
{
    auto const* focused_element = this->focused_element();
    if (!focused_element)
        return nullptr;

    Optional<HTML::FormAssociatedTextControlElement const&> target {};
    if (is<HTML::HTMLInputElement>(*focused_element))
        target = static_cast<HTML::HTMLInputElement const&>(*focused_element);
    else if (is<HTML::HTMLTextAreaElement>(*focused_element))
        target = static_cast<HTML::HTMLTextAreaElement const&>(*focused_element);

    if (target.has_value())
        return target->cursor_position();

    if (focused_element->is_editable_or_editing_host())
        return m_selection->cursor_position();

    return nullptr;
}

void Document::reset_cursor_blink_cycle()
{
    m_cursor_blink_state = true;
    m_cursor_blink_timer->restart();
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#doc-container-document
GC::Ptr<DOM::Document> Document::container_document() const
{
    // 1. If document's node navigable is null, then return null.
    auto node_navigable = navigable();
    if (!node_navigable)
        return nullptr;

    // 2. Return document's node navigable's container document.
    return node_navigable->container_document();
}

GC::Ptr<HTML::Navigable> Document::cached_navigable()
{
    return m_cached_navigable.ptr();
}

void Document::set_cached_navigable(GC::Ptr<HTML::Navigable> navigable)
{
    m_cached_navigable = navigable.ptr();
}

void Document::set_needs_display(InvalidateDisplayList should_invalidate_display_list)
{
    set_needs_display(viewport_rect(), should_invalidate_display_list);
}

void Document::set_needs_display(CSSPixelRect const&, InvalidateDisplayList should_invalidate_display_list)
{
    // FIXME: Ignore updates outside the visible viewport rect.
    //        This requires accounting for fixed-position elements in the input rect, which we don't do yet.

    if (should_invalidate_display_list == InvalidateDisplayList::Yes) {
        invalidate_display_list();
    }

    auto navigable = this->navigable();
    if (!navigable)
        return;

    if (navigable->is_traversable()) {
        navigable->traversable_navigable()->set_needs_repaint();
        Web::HTML::main_thread_event_loop().schedule();
        return;
    }

    if (auto container = navigable->container()) {
        container->document().set_needs_display(should_invalidate_display_list);
    }
}

void Document::invalidate_display_list()
{
    m_cached_display_list.clear();

    auto navigable = this->navigable();
    if (!navigable)
        return;

    if (auto container = navigable->container()) {
        container->document().invalidate_display_list();
    }
}

RefPtr<Painting::DisplayList> Document::record_display_list(PaintConfig config)
{
    if (m_cached_display_list && m_cached_display_list_paint_config == config)
        return m_cached_display_list;

    auto display_list = Painting::DisplayList::create();
    Painting::DisplayListRecorder display_list_recorder(display_list);

    // https://drafts.csswg.org/css-color-adjust-1/#color-scheme-effect
    // On the root element, the used color scheme additionally must affect the surface color of the canvas, and the viewport’s scrollbars.
    auto color_scheme = CSS::PreferredColorScheme::Light;
    if (auto* html_element = this->html_element(); html_element && html_element->layout_node()) {
        if (html_element->layout_node()->computed_values().color_scheme() == CSS::PreferredColorScheme::Dark)
            color_scheme = CSS::PreferredColorScheme::Dark;
    }

    // .. in the case of embedded documents typically rendered over a transparent canvas
    // (such as provided via an HTML iframe element), if the used color scheme of the element
    // and the used color scheme of the embedded document’s root element do not match,
    // then the UA must use an opaque canvas of the Canvas color appropriate to the
    // embedded document’s used color scheme instead of a transparent canvas.
    bool opaque_canvas = false;
    if (auto container_element = navigable()->container(); container_element && container_element->layout_node()) {
        auto container_scheme = container_element->layout_node()->computed_values().color_scheme();
        if (container_scheme == CSS::PreferredColorScheme::Auto)
            container_scheme = CSS::PreferredColorScheme::Light;

        opaque_canvas = container_scheme != color_scheme;
    }

    if (config.canvas_fill_rect.has_value()) {
        display_list_recorder.fill_rect(config.canvas_fill_rect.value(), CSS::SystemColor::canvas(color_scheme));
    }

    auto viewport_rect = page().css_to_device_rect(this->viewport_rect());
    Gfx::IntRect bitmap_rect { {}, viewport_rect.size().to_type<int>() };

    if (opaque_canvas)
        display_list_recorder.fill_rect(bitmap_rect, CSS::SystemColor::canvas(color_scheme));

    display_list_recorder.fill_rect(bitmap_rect, background_color());
    if (!paintable()) {
        VERIFY_NOT_REACHED();
    }

    Web::PaintContext context(display_list_recorder, page().palette(), page().client().device_pixels_per_css_pixel());
    context.set_device_viewport_rect(viewport_rect);
    context.set_should_show_line_box_borders(config.should_show_line_box_borders);
    context.set_should_paint_overlay(config.paint_overlay);
    context.set_has_focus(config.has_focus);

    update_paint_and_hit_testing_properties_if_needed();

    auto& viewport_paintable = *paintable();

    viewport_paintable.refresh_scroll_state();

    viewport_paintable.paint_all_phases(context);

    display_list->set_device_pixels_per_css_pixel(page().client().device_pixels_per_css_pixel());
    display_list->set_scroll_state(viewport_paintable.scroll_state());

    m_cached_display_list = display_list;
    m_cached_display_list_paint_config = config;

    return display_list;
}

Unicode::Segmenter& Document::grapheme_segmenter() const
{
    if (!m_grapheme_segmenter)
        m_grapheme_segmenter = Unicode::Segmenter::create(Unicode::SegmenterGranularity::Grapheme);
    return *m_grapheme_segmenter;
}

Unicode::Segmenter& Document::word_segmenter() const
{
    if (!m_word_segmenter)
        m_word_segmenter = Unicode::Segmenter::create(Unicode::SegmenterGranularity::Word);
    return *m_word_segmenter;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#steps-to-fire-beforeunload
Document::StepsToFireBeforeunloadResult Document::steps_to_fire_beforeunload(bool unload_prompt_shown)
{
    // 1. Let unloadPromptCanceled be false.
    auto unload_prompt_canceled = false;

    // 2. Increase the document's unload counter by 1.
    m_unload_counter++;

    // 3. Increase document's relevant agent's event loop's termination nesting level by 1.
    auto& event_loop = *HTML::relevant_agent(*this).event_loop;
    event_loop.increment_termination_nesting_level();

    // 4. Let eventFiringResult be the result of firing an event named beforeunload at document's relevant global object,
    //    using BeforeUnloadEvent, with the cancelable attribute initialized to true.
    auto& global_object = HTML::relevant_global_object(*this);
    auto& window = as<HTML::Window>(global_object);
    auto beforeunload_event = HTML::BeforeUnloadEvent::create(realm(), HTML::EventNames::beforeunload);
    beforeunload_event->set_cancelable(true);
    auto event_firing_result = window.dispatch_event(*beforeunload_event);

    // 5. Decrease document's relevant agent's event loop's termination nesting level by 1.
    event_loop.decrement_termination_nesting_level();

    // FIXME: 6. If all of the following are true:
    if (false &&
        //    - unloadPromptShown is false;
        !unload_prompt_shown
        //    - document's active sandboxing flag set does not have its sandboxed modals flag set;
        && !has_flag(document().active_sandboxing_flag_set(), HTML::SandboxingFlagSet::SandboxedModals)
        //    - document's relevant global object has sticky activation;
        && window.has_sticky_activation()
        //    - eventFiringResult is false, or the returnValue attribute of event is not the empty string; and
        && (!event_firing_result || !beforeunload_event->return_value().is_empty())
        //    - FIXME: showing an unload prompt is unlikely to be annoying, deceptive, or pointless
    ) {
        // FIXME: 1. Set unloadPromptShown to true.
        // FIXME: 2. Invoke WebDriver BiDi user prompt opened with document's relevant global object, "beforeunload", and "".
        // FIXME: 3. Ask the user to confirm that they wish to unload the document, and pause while waiting for the user's response.
        // FIXME: 4. If the user did not confirm the page navigation, set unloadPromptCanceled to true.
        // FIXME: 5. Invoke WebDriver BiDi user prompt closed with document's relevant global object and true if unloadPromptCanceled is false or false otherwise.
    }

    // 7. Decrease document's unload counter by 1.
    m_unload_counter--;

    // 8. Return (unloadPromptShown, unloadPromptCanceled).
    return { unload_prompt_shown, unload_prompt_canceled };
}

// https://w3c.github.io/webappsec-csp/#run-document-csp-initialization
void Document::run_csp_initialization() const
{
    // 1. For each policy of document’s policy container's CSP list:
    for (auto policy : policy_container()->csp_list->policies()) {
        // 1. For each directive of policy:
        for (auto directive : policy->directives()) {
            // 1. Execute directive’s initialization algorithm on document, and assert: its returned value is "Allowed".
            auto result = directive->initialization(GC::Ref { *this }, policy);
            VERIFY(result == ContentSecurityPolicy::Directives::Directive::Result::Allowed);
        }
    }
}

WebIDL::CallbackType* Document::onreadystatechange()
{
    return event_handler_attribute(HTML::EventNames::readystatechange);
}

void Document::set_onreadystatechange(WebIDL::CallbackType* value)
{
    set_event_handler_attribute(HTML::EventNames::readystatechange, value);
}

WebIDL::CallbackType* Document::onvisibilitychange()
{
    return event_handler_attribute(HTML::EventNames::visibilitychange);
}

void Document::set_onvisibilitychange(WebIDL::CallbackType* value)
{
    set_event_handler_attribute(HTML::EventNames::visibilitychange, value);
}

ElementByIdMap& Document::element_by_id() const
{
    if (!m_element_by_id)
        m_element_by_id = make<ElementByIdMap>();
    return *m_element_by_id;
}

GC::Ptr<Element> ElementByIdMap::get(FlyString const& element_id) const
{
    if (auto elements = m_map.get(element_id); elements.has_value() && !elements->is_empty()) {
        if (auto element = elements->first(); element.has_value())
            return *element;
    }
    return {};
}

StringView to_string(SetNeedsLayoutReason reason)
{
    switch (reason) {
#define ENUMERATE_SET_NEEDS_LAYOUT_REASON(e) \
    case SetNeedsLayoutReason::e:            \
        return #e##sv;
        ENUMERATE_SET_NEEDS_LAYOUT_REASONS(ENUMERATE_SET_NEEDS_LAYOUT_REASON)
#undef ENUMERATE_SET_NEEDS_LAYOUT_REASON
    }
    VERIFY_NOT_REACHED();
}

StringView to_string(InvalidateLayoutTreeReason reason)
{
    switch (reason) {
#define ENUMERATE_INVALIDATE_LAYOUT_TREE_REASON(e) \
    case InvalidateLayoutTreeReason::e:            \
        return #e##sv;
        ENUMERATE_INVALIDATE_LAYOUT_TREE_REASONS(ENUMERATE_INVALIDATE_LAYOUT_TREE_REASON)
#undef ENUMERATE_INVALIDATE_LAYOUT_TREE_REASON
    }
    VERIFY_NOT_REACHED();
}

StringView to_string(UpdateLayoutReason reason)
{
    switch (reason) {
#define ENUMERATE_UPDATE_LAYOUT_REASON(e) \
    case UpdateLayoutReason::e:           \
        return #e##sv;
        ENUMERATE_UPDATE_LAYOUT_REASONS(ENUMERATE_UPDATE_LAYOUT_REASON)
#undef ENUMERATE_UPDATE_LAYOUT_REASON
    }
    VERIFY_NOT_REACHED();
}

}
