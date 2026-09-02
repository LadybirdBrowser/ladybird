/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021-2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2025, Simon Farre <simon.farre.cx@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Bitmap.h>
#include <AK/CharacterTypes.h>
#include <AK/Debug.h>
#include <AK/GenericLexer.h>
#include <AK/InsertionSort.h>
#include <AK/JsonObjectSerializer.h>
#include <AK/NeverDestroyed.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <AK/StringBuilder.h>
#include <AK/Time.h>
#include <AK/Utf16StringBuilder.h>
#include <AK/Utf16View.h>
#include <AK/Utf8View.h>
#include <LibCore/Timer.h>
#include <LibGC/ConservativeVector.h>
#include <LibGC/Heap.h>
#include <LibGC/RootHashTable.h>
#include <LibGC/RootVector.h>
#include <LibHTTP/Cookie/Cookie.h>
#include <LibHTTP/Cookie/ParsedCookie.h>
#include <LibJS/Console.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibTextCodec/Decoder.h>
#include <LibURL/Origin.h>
#include <LibURL/Parser.h>
#include <LibUnicode/Segmenter.h>
#include <LibWeb/Animations/Animation.h>
#include <LibWeb/Animations/AnimationPlaybackEvent.h>
#include <LibWeb/Animations/AnimationTimeline.h>
#include <LibWeb/Animations/DocumentTimeline.h>
#include <LibWeb/Animations/TimeValue.h>
#include <LibWeb/Bindings/Document.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/CSS/Angle.h>
#include <LibWeb/CSS/AnimationEvent.h>
#include <LibWeb/CSS/CSSAnimation.h>
#include <LibWeb/CSS/CSSImportRule.h>
#include <LibWeb/CSS/CSSPropertyRule.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/CSSTransition.h>
#include <LibWeb/CSS/ComputedStyleWorkingSet.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/CustomPropertyRegistration.h>
#include <LibWeb/CSS/FontComputer.h>
#include <LibWeb/CSS/FontFaceSet.h>
#include <LibWeb/CSS/HypotheticalElement.h>
#include <LibWeb/CSS/Invalidation/AdoptedStyleSheetInvalidator.h>
#include <LibWeb/CSS/Invalidation/ContainerQueryInvalidator.h>
#include <LibWeb/CSS/Invalidation/ElementStateInvalidator.h>
#include <LibWeb/CSS/Invalidation/LinkInvalidator.h>
#include <LibWeb/CSS/Invalidation/MediaQueryInvalidator.h>
#include <LibWeb/CSS/Invalidation/PseudoClassInvalidator.h>
#include <LibWeb/CSS/Length.h>
#include <LibWeb/CSS/MediaQueryList.h>
#include <LibWeb/CSS/MediaQueryListEvent.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/SelectorMatching.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StyleSheetIdentifier.h>
#include <LibWeb/CSS/StyleSheetList.h>
#include <LibWeb/CSS/StyleValues/ColorSchemeStyleValue.h>
#include <LibWeb/CSS/StyleValues/ComputationContext.h>
#include <LibWeb/CSS/StyleValues/GuaranteedInvalidStyleValue.h>
#include <LibWeb/CSS/StyleValues/ImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/OpacityValueStyleValue.h>
#include <LibWeb/CSS/StyleValues/RandomValueSharingStyleValue.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/CSS/SystemColor.h>
#include <LibWeb/CSS/TransitionEvent.h>
#include <LibWeb/CSS/VisualViewport.h>
#include <LibWeb/Compositor/VisualAnimation.h>
#include <LibWeb/ComputedValuesRustFFI.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>
#include <LibWeb/ContentSecurityPolicy/Policy.h>
#include <LibWeb/ContentSecurityPolicy/PolicyList.h>
#include <LibWeb/DOM/AbstractElement.h>
#include <LibWeb/DOM/AccessibilityTreeNode.h>
#include <LibWeb/DOM/AdoptedStyleSheets.h>
#include <LibWeb/DOM/Attr.h>
#include <LibWeb/DOM/BindingsGlue.h>
#include <LibWeb/DOM/CDATASection.h>
#include <LibWeb/DOM/CaretPosition.h>
#include <LibWeb/DOM/Comment.h>
#include <LibWeb/DOM/CustomEvent.h>
#include <LibWeb/DOM/DOMImplementation.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentFragment.h>
#include <LibWeb/DOM/DocumentObserver.h>
#include <LibWeb/DOM/DocumentOrShadowRoot.h>
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
#include <LibWeb/DOM/SelectorQuery.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/DOM/TreeWalker.h>
#include <LibWeb/DOM/Utils.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/Dump.h>
#include <LibWeb/Editing/EditingHistory.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/FetchRecord.h>
#include <LibWeb/Fetch/Infrastructure/FetchTimingInfo.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/FileAPI/BlobURLStore.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/BeforeUnloadEvent.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/BrowsingContextGroup.h>
#include <LibWeb/HTML/CustomElements/CustomElementDefinition.h>
#include <LibWeb/HTML/CustomElements/CustomElementReactionNames.h>
#include <LibWeb/HTML/CustomElements/CustomElementRegistry.h>
#include <LibWeb/HTML/DOMStringList.h>
#include <LibWeb/HTML/DecodedImageData.h>
#include <LibWeb/HTML/DocumentState.h>
#include <LibWeb/HTML/DragEvent.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/Focus.h>
#include <LibWeb/HTML/HTMLAllCollection.h>
#include <LibWeb/HTML/HTMLAnchorElement.h>
#include <LibWeb/HTML/HTMLAreaElement.h>
#include <LibWeb/HTML/HTMLBaseElement.h>
#include <LibWeb/HTML/HTMLBodyElement.h>
#include <LibWeb/HTML/HTMLDialogElement.h>
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
#include <LibWeb/HTML/History.h>
#include <LibWeb/HTML/ListOfAvailableImages.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/LocalTraversableNavigable.h>
#include <LibWeb/HTML/Location.h>
#include <LibWeb/HTML/MessageEvent.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/HTML/Navigation.h>
#include <LibWeb/HTML/NavigationParams.h>
#include <LibWeb/HTML/Numbers.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWeb/HTML/PopStateEvent.h>
#include <LibWeb/HTML/RadioButtonGroupRegistry.h>
#include <LibWeb/HTML/Scripting/Agent.h>
#include <LibWeb/HTML/Scripting/ClassicScript.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Scripting/WindowEnvironmentSettingsObject.h>
#include <LibWeb/HTML/Scripting/WindowRealm.h>
#include <LibWeb/HTML/SharedResourceRequest.h>
#include <LibWeb/HTML/Storage.h>
#include <LibWeb/HTML/StorageEvent.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/HighResolutionTime/Performance.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/Infra/CharacterTypes.h>
#include <LibWeb/Infra/SerializedURL.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/IntersectionObserver/IntersectionObserver.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/LayoutRustBridge.h>
#include <LibWeb/Layout/NodeArena.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Layout/TextOffsetMapping.h>
#include <LibWeb/Layout/TreeBuilder.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Loader/ContentBlocker.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/NavigationTiming/PerformanceNavigationTiming.h>
#include <LibWeb/Page/EventHandler.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/ChromeWidget.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/DisplayListCommand.h>
#include <LibWeb/Painting/DocumentPaintState.h>
#include <LibWeb/Painting/HitTestDisplayList.h>
#include <LibWeb/Painting/PaintableTypes.h>
#include <LibWeb/Painting/PaintingRustBridge.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/ResizeObserver/ResizeObserver.h>
#include <LibWeb/ResizeObserver/ResizeObserverEntry.h>
#include <LibWeb/SVG/SVGDecodedImageData.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGSVGElement.h>
#include <LibWeb/SVG/SVGScriptElement.h>
#include <LibWeb/SVG/SVGStyleElement.h>
#include <LibWeb/SVG/SVGTitleElement.h>
#include <LibWeb/SVG/SVGUseElement.h>
#include <LibWeb/Selection/Selection.h>
#include <LibWeb/TrustedTypes/RequireTrustedTypesForDirective.h>
#include <LibWeb/TrustedTypes/TrustedTypePolicy.h>
#include <LibWeb/UIEvents/CompositionEvent.h>
#include <LibWeb/UIEvents/EventNames.h>
#include <LibWeb/UIEvents/FocusEvent.h>
#include <LibWeb/UIEvents/KeyCode.h>
#include <LibWeb/UIEvents/KeyboardEvent.h>
#include <LibWeb/UIEvents/MouseButton.h>
#include <LibWeb/UIEvents/MouseEvent.h>
#include <LibWeb/UIEvents/PointerEvent.h>
#include <LibWeb/UIEvents/PointerTypes.h>
#include <LibWeb/UIEvents/TextEvent.h>
#include <LibWeb/ViewTransition/ViewTransition.h>
#include <LibWeb/WebDriver/UserPrompt.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/ObservableArray.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWeb/XHR/XMLHttpRequest.h>
#include <LibWeb/XPath/XPath.h>

namespace Web::DOM {

static CSS::ComputedValuesFFI::FfiUtf16View ffi_utf16_view(Utf16View view)
{
    return {
        .ascii = view.has_ascii_storage() ? reinterpret_cast<u8 const*>(view.ascii_span().data()) : nullptr,
        .utf16 = view.has_ascii_storage() ? nullptr : reinterpret_cast<u16 const*>(view.utf16_span().data()),
        .length = view.length_in_code_units(),
    };
}

void Document::register_valid_html_collection_cache(HTMLCollectionAttributeInvalidationType type) const
{
    VERIFY(type != HTMLCollectionAttributeInvalidationType::Count);
    auto index = to_underlying(type);
    ++m_html_collection_attribute_invalidation_type_counts[index];
    m_html_collection_attribute_invalidation_types |= HTMLCollectionCacheRegistration::attribute_invalidation_type_mask(type);
}

void Document::unregister_valid_html_collection_cache(HTMLCollectionAttributeInvalidationType type) const
{
    VERIFY(type != HTMLCollectionAttributeInvalidationType::Count);
    auto index = to_underlying(type);
    VERIFY(m_html_collection_attribute_invalidation_type_counts[index] > 0);
    if (--m_html_collection_attribute_invalidation_type_counts[index] == 0)
        m_html_collection_attribute_invalidation_types &= ~HTMLCollectionCacheRegistration::attribute_invalidation_type_mask(type);
}

Document::HTMLCollectionAttributeInvalidationTypes Document::html_collection_attribute_invalidation_types_for_attribute(Utf16FlyString const& local_name, Optional<Utf16FlyString> const& namespace_) const
{
    constexpr auto none = HTMLCollectionCacheRegistration::attribute_invalidation_type_mask(HTMLCollectionAttributeInvalidationType::None);
    constexpr auto class_ = HTMLCollectionCacheRegistration::attribute_invalidation_type_mask(HTMLCollectionAttributeInvalidationType::Class);
    constexpr auto name = HTMLCollectionCacheRegistration::attribute_invalidation_type_mask(HTMLCollectionAttributeInvalidationType::Name);
    constexpr auto id_or_name = HTMLCollectionCacheRegistration::attribute_invalidation_type_mask(HTMLCollectionAttributeInvalidationType::IdOrName);
    constexpr auto href = HTMLCollectionCacheRegistration::attribute_invalidation_type_mask(HTMLCollectionAttributeInvalidationType::Href);
    constexpr auto form_controls = HTMLCollectionCacheRegistration::attribute_invalidation_type_mask(HTMLCollectionAttributeInvalidationType::FormControls);

    auto registered_types = m_html_collection_attribute_invalidation_types;
    auto attribute_sensitive_types = registered_types & ~none;
    if (attribute_sensitive_types == 0)
        return 0;

    HTMLCollectionAttributeInvalidationTypes types = 0;
    if (!namespace_.has_value()) {
        if ((attribute_sensitive_types & class_) && local_name == HTML::AttributeNames::class_)
            types |= class_;
        if ((attribute_sensitive_types & name) && local_name == HTML::AttributeNames::name)
            types |= name;
        if ((attribute_sensitive_types & id_or_name) && local_name.is_one_of(HTML::AttributeNames::id, HTML::AttributeNames::name))
            types |= id_or_name;
        if ((attribute_sensitive_types & href) && local_name == HTML::AttributeNames::href)
            types |= href;
        if ((attribute_sensitive_types & form_controls) && local_name == HTML::AttributeNames::type)
            types |= form_controls;
    }
    return types;
}

static size_t retain_registered_property_utf16_fly_string(u16 const* code_units, size_t length)
{
    return Utf16FlyString::from_utf16(Utf16View { reinterpret_cast<char16_t const*>(code_units), length }).to_raw_leaked();
}

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
    auto browsing_context_and_document = HTML::create_a_new_top_level_browsing_context_and_document(browsing_context.page());
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

// https://html.spec.whatwg.org/multipage/document-lifecycle.html#initialise-the-document-object
WebIDL::ExceptionOr<GC::Ref<Document>> Document::create_and_initialize(Type type, Utf16FlyString content_type, HTML::NavigationParams const& navigation_params)
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
    VERIFY(browsing_context->active_document());
    if (browsing_context->active_document()->is_initial_about_blank()
        && browsing_context->active_document()->origin().is_same_origin_domain(navigation_params.origin)) {
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
        auto realm_execution_context = HTML::create_window_realm(window, *browsing_context);

        // 6. Set window to the global object of realmExecutionContext's Realm component.
        window = HTML::window_from_global_object(realm_execution_context->realm->global_object());
        VERIFY(window);

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
            top_level_origin = parent_environment.top_level_origin.value();
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
    auto timing_info = Fetch::Infrastructure::FetchTimingInfo::create();
    if (navigation_params.fetch_controller && navigation_params.fetch_controller->timing_info()) {
        timing_info = *navigation_params.fetch_controller->timing_info();
        load_timing_info.navigation_start_time = timing_info->start_time();
    } else {
        // AD-HOC: Non-fetch navigations do not have timing info, so use the time at which the response was created.
        auto response_creation_time = navigation_params.response->monotonic_response_time().nanoseconds() / 1e6;
        load_timing_info.navigation_start_time = HighResolutionTime::coarsen_time(response_creation_time, HTML::relevant_settings_object(*window).cross_origin_isolated_capability());
        timing_info->set_start_time(load_timing_info.navigation_start_time);
        timing_info->set_post_redirect_start_time(load_timing_info.navigation_start_time);
        timing_info->set_final_network_request_start_time(load_timing_info.navigation_start_time);
        timing_info->set_final_network_response_start_time(load_timing_info.navigation_start_time);
        timing_info->set_end_time(load_timing_info.navigation_start_time);
    }

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
    //    custom element registry: A new CustomElementRegistry object.
    auto document = HTML::HTMLDocument::create(browsing_context->page(), *window);
    document->set_window(*window);
    document->m_type = type;
    document->m_content_type = move(content_type);
    document->set_origin(navigation_params.origin);
    document->set_browsing_context(browsing_context);
    document->m_policy_container = navigation_params.policy_container;
    document->m_active_sandboxing_flag_set = navigation_params.final_sandboxing_flag_set;
    document->m_navigation_id = navigation_params.id;
    document->set_load_timing_info(load_timing_info);
    document->m_about_base_url = navigation_params.about_base_url;
    document->set_url(*creation_url);
    document->m_readiness = HTML::DocumentReadyState::Loading;
    document->set_allow_declarative_shadow_roots(HTML::HTMLParser::AllowDeclarativeShadowRoots::Yes);
    document->set_custom_element_registry(HTML::CustomElementRegistry::create_global(*document));

    // NOTE: Non-standard: Pull out the Last-Modified header for use in the lastModified property.
    if (auto maybe_last_modified = navigation_params.response->header_list()->get("Last-Modified"sv); maybe_last_modified.has_value()) {
        // rfc9110, 8.8.2: The Last-Modified header field must be in GMT time zone.
        // document->m_last_modified is in local time zone.
        document->m_last_modified = AK::UnixDateTime::parse("%a, %d %b %Y %H:%M:%S GMT"sv, maybe_last_modified.value(), true);
    }

    // NOTE: Non-standard: Pull out the Content-Language header to determine the document's language.
    if (auto maybe_http_content_language = navigation_params.response->header_list()->get("Content-Language"sv); maybe_http_content_language.has_value()) {
        if (auto maybe_content_language = Utf16String::try_from_utf8(maybe_http_content_language->view()); !maybe_content_language.is_error())
            document->m_http_content_language = maybe_content_language.release_value();
    }

    // 10. Set window's associated Document to document.
    window->set_associated_document(*document);

    bool has_cross_origin_redirects = false;
    if (!navigation_params.response->url_list().is_empty()) {
        auto initial_origin = navigation_params.response->url_list().first().origin();
        for (auto const& url : navigation_params.response->url_list()) {
            if (!url.origin().is_same_origin(initial_origin)) {
                has_cross_origin_redirects = true;
                break;
            }
        }
    }
    auto redirect_count = navigation_params.request && !has_cross_origin_redirects ? navigation_params.request->redirect_count() : 0;
    NavigationTiming::PerformanceNavigationTiming::create_navigation_timing_entry(
        *document,
        timing_info,
        redirect_count,
        navigation_params.navigation_timing_type,
        navigation_params.response->cache_state(),
        navigation_params.response->body_info(),
        navigation_params.response->status());

    // 11. Set document's internal ancestor origin objects list to the result of running the internal ancestor origin
    //     objects list creation steps given document and navigationParams's iframe element referrer policy.
    document->set_internal_ancestor_origin_objects_list(document->internal_ancestor_origin_objects_list_creation_steps(navigation_params.iframe_element_referrer_policy));

    // 12. Set document's ancestor origins list to the result of running the ancestor origins list creation steps given
    //     document.
    document->set_ancestor_origins_list(document->ancestor_origins_list_creation_steps());

    // 13. Run CSP initialization for a Document given document.
    document->run_csp_initialization();

    // 14. If navigationParams's request is non-null, then:
    if (navigation_params.request) {
        // 1. Set document's referrer to the empty string.
        document->m_referrer = {};

        // 2. Let referrer be navigationParams's request's referrer.
        auto const& referrer = navigation_params.request->referrer();

        // 3. If referrer is a URL record, then set document's referrer to the serialization of referrer.
        if (referrer.has<URL::URL>()) {
            document->m_referrer = utf16_string_from_url_ascii(referrer.get<URL::URL>().serialize());
        }
    }

    // AD-HOC: Retain the navigation fetch controller so aborting the document can cancel the main resource after
    //         response commitment. Navigation fetches are not included in the document's subresource fetch group.
    if (navigation_params.fetch_controller)
        document->m_ongoing_navigation_fetch_controller = navigation_params.fetch_controller;

    // FIXME: 15. If navigationParams's fetch controller is not null:
    //            1. Let fullTimingInfo be the result of extracting the full timing info from navigationParams's fetch controller.
    //            2. Let redirectCount be 0.
    //            3. If navigationParams's response's has cross-origin redirects is false, or all of the following are true:
    //                - navigationParams's request's client is null, or navigationParams's request's referrer is not "no-referrer", and
    //                - navigation TAO check given navigationParams's response and navigationParams's origin returns success,
    //               then set redirectCount to navigationParams's request's redirect count.
    //            4. Create the navigation timing entry for document, given fullTimingInfo, redirectCount, navigationTimingType,
    //               navigationParams's response's service worker timing info, and navigationParams's response's body info.

    // FIXME: 16. Create the navigation timing entry for document, with navigationParams's response's timing info,
    //        redirectCount, navigationParams's navigation timing type, and navigationParams's response's service
    //        worker timing info.

    // 17. If navigationParams's response has a `Refresh` header, then:
    if (auto maybe_refresh = navigation_params.response->header_list()->get("Refresh"sv); maybe_refresh.has_value()) {
        // 1. Let value be the isomorphic decoding of the value of the header.
        auto value = TextCodec::isomorphic_decode_to_utf16(maybe_refresh.value());

        // 2. Run the shared declarative refresh steps with document and value.
        document->shared_declarative_refresh_steps(value, nullptr);
    }

    // FIXME: 18. If navigationParams's commit early hints is not null, then call navigationParams's commit early hints
    //        with document.

    // FIXME: 19. Process link headers given document, navigationParams's response, and "pre-media".

    // FIXME: 20. If navigationParams's navigable is a top-level traversable, then process the `Speculation-Rules`
    //        header given document and navigationParams's response .

    // FIXME: 21. Potentially free deferred fetch quota for document.

    // 22. Return document.
    return document;
}

GC::Ref<Document> Document::create(Page& page, GC::Ref<EventTarget> relevant_global_event_target, URL::URL const& url)
{
    auto document = GC::Heap::the().allocate<Document>(page, relevant_global_event_target, url);
    document->initialize_document();
    return document;
}

GC::Ref<Document> Document::create_for_constructor(JS::Object& relevant_global_object)
{
    auto& window = HTML::relevant_window(relevant_global_object);
    auto document = DOM::Document::create(window.page(), window);
    document->set_origin(window.associated_document().origin());
    return document;
}

GC::Ref<Document> Document::create_for_fragment_parsing(Page& page, GC::Ref<EventTarget> relevant_global_event_target)
{
    auto document = GC::Heap::the().allocate<Document>(page, relevant_global_event_target, URL::about_blank(), TemporaryDocumentForFragmentParsing::Yes);
    document->initialize_document();
    return document;
}

Document::Document(Page& page, GC::Ref<EventTarget> relevant_global_event_target, URL::URL const& url, TemporaryDocumentForFragmentParsing temporary_document_for_fragment_parsing)
    : ParentNode(*this, NodeType::DOCUMENT_NODE)
    , m_page(page)
    , m_style_computer(GC::Heap::the().allocate<CSS::StyleComputer>(*this))
    , m_font_computer(GC::Heap::the().allocate<CSS::FontComputer>(*this))
    , m_url(url)
    , m_relevant_global_event_target(relevant_global_event_target)
    , m_chrome_widget_registry(make_ref_counted<Painting::ChromeWidgetRegistry>())
    , m_fonts(CSS::FontFaceSet::create(relevant_settings_object()))
    , m_temporary_document_for_fragment_parsing(temporary_document_for_fragment_parsing == TemporaryDocumentForFragmentParsing::Yes)
    , m_editing_host_manager(EditingHostManager::create(*this))
    , m_dynamic_view_transition_style_sheet(parse_css_stylesheet(CSS::Parser::ParsingParams {}, ""sv, {}))
    , m_style_scope(*this)
{
    m_rust_custom_property_registry = CSS::ComputedValuesFFI::rust_custom_property_registry_create();

    m_is_decoded_svg = m_page->client().is_svg_page_client();

    HTML::main_thread_event_loop().register_document({}, *this);
}

Document::~Document() = default;

void Document::synchronize_dirty_style_attributes()
{
    if (!m_has_dirty_style_attributes)
        return;

    m_has_dirty_style_attributes = false;
    for_each_shadow_including_descendant([](Node& node) {
        if (auto* element = as_if<Element>(node))
            element->synchronize_all_attributes();
        return TraversalDecision::Continue;
    });
}

Layout::NodeArena& Document::layout_node_arena()
{
    // Created on first layout node so documents that never build a layout tree (e.g. temporary
    // fragment-parsing documents) skip the Rust arena round-trip entirely.
    if (!m_layout_node_arena) {
        m_layout_node_arena = make_ref_counted<Layout::NodeArena>();
        Layout::RustFFI::layout_arena_set_chrome_state_callback(
            m_layout_node_arena->handle(), this,
            [](void* context, Layout::RustFFI::NodeSlotId slot, Layout::RustFFI::PaintableRowResetKind kind) {
                auto& document = *static_cast<Document*>(context);
                document.chrome_widget_registry().drop_widgets_for_slot(slot);
                if (kind == Layout::RustFFI::PaintableRowResetKind::Recommitted && Painting::viewport_row_slot(document).index == slot.index)
                    document.paint_state().viewport_row_was_reset(document);
            });
    }
    return *m_layout_node_arena;
}

void Document::reset_style_invalidation_counters() const
{
    m_style_invalidation_counters = {};
    CSS::reset_longhand_wrappers_minted();
}

void Document::record_layout_tree_build(u64 rebuilt_subtree_root_count, bool escaped_rebuild_roots)
{
    ++m_layout_tree_build_stats.builds;
    m_layout_tree_build_stats.last_build_rebuilt_subtree_roots = rebuilt_subtree_root_count;
    m_layout_tree_build_stats.last_build_escaped_rebuild_roots = escaped_rebuild_roots;
}

void Document::finalize()
{
    stop_compositor_animation_timers();
    if (m_layout_node_arena)
        Layout::RustFFI::layout_arena_clear_chrome_state_callback(m_layout_node_arena->handle());
    CSS::ComputedValuesFFI::rust_custom_property_registry_destroy(m_rust_custom_property_registry);
    Base::finalize();
    HTML::main_thread_event_loop().unregister_document({}, *this);
}

void Document::initialize_document()
{
    m_selection = Selection::Selection::create(*this);

    m_list_of_available_images = HTML::ListOfAvailableImages::create();

    page().client().page_did_create_new_document(*this);

    ensure_cookie_version_index(m_url);
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
    m_style_scope.visit_edges(visitor);
    visitor.visit(m_pending_css_import_rules);
    visitor.visit(m_page);
    visitor.visit(m_window);
    visitor.visit(m_relevant_global_event_target);
    visitor.visit(m_style_sheets);
    visitor.visit(m_hovered_node);
    visitor.visit(m_inspected_node);
    visitor.visit(m_highlighted_node);
    for (auto const& flexbox_highlight : m_flexbox_highlights)
        visitor.visit(flexbox_highlight.node);
    for (auto const& grid_highlight : m_grid_highlights)
        visitor.visit(grid_highlight.node);
    visitor.visit(m_active_favicon);
    visitor.visit(m_browsing_context);
    visitor.visit(m_focused_area);
    visitor.visit(m_active_element);
    visitor.visit(m_target_element);
    visitor.visit(m_autofocus_candidates);
    visitor.visit(m_implementation);
    visitor.visit(m_current_script);
    visitor.visit(m_associated_inert_template_document);
    visitor.visit(m_appropriate_template_contents_owner_document);
    visitor.visit(m_pending_parsing_blocking_script);
    visitor.visit(m_pending_parsing_blocking_svg_script);
    visitor.visit(m_history);
    visitor.visit(m_html_parser_end_state);
    visitor.visit(m_ongoing_navigation_fetch_controller);
    visitor.visit(m_navigation_timing_entry);
    visitor.visit(m_style_computer);
    visitor.visit(m_font_computer);
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
    visitor.visit(m_default_timeline);
    visitor.visit(m_scripts_to_execute_when_parsing_has_finished);
    visitor.visit(m_scripts_to_execute_in_order_as_soon_as_possible);
    visitor.visit(m_scripts_to_execute_as_soon_as_possible);
    visitor.visit(m_node_iterators);
    visitor.visit(m_document_observers_being_notified);
    for (auto& pending_scroll_event : m_pending_scroll_events)
        visitor.visit(pending_scroll_event.event_target);
    visitor.visit(m_query_containers_needing_container_query_evaluation_after_layout);
    visitor.visit(m_list_owners_pending_item_renumber);
    visitor.visit(m_list_owners_with_stale_item_counters);

    visitor.visit(m_shared_resource_requests);
    for (auto& resource : m_css_image_resources)
        resource.value->visit_edges(visitor);

    visitor.visit(m_associated_animation_timelines);
    visitor.visit(m_list_of_available_images);
    for (auto& it : m_map_of_preloaded_resources)
        visitor.visit(it.value);

    for (auto* form_associated_element : m_form_associated_elements_with_form_attribute)
        visitor.visit(form_associated_element->form_associated_element_to_html_element());

    visitor.visit(m_radio_button_group_registry);

    visitor.visit(m_potentially_named_elements);
    m_anchor_name_map.visit_edges(visitor);
    if (m_query_selector_result_cache)
        m_query_selector_result_cache->visit_edges(visitor);

    for (auto& event : m_pending_animation_event_queue) {
        visitor.visit(event.event);
        visitor.visit(event.animation);
        visitor.visit(event.target);
    }
    for (auto& event : m_provisional_animation_event_queue) {
        visitor.visit(event.event);
        visitor.visit(event.animation);
        visitor.visit(event.target);
    }

    for (auto& event : m_pending_fullscreen_events) {
        visitor.visit(event.element);
    }

    visitor.visit(m_adopted_style_sheets);
    visitor.visit(m_script_blocking_style_sheet_set);
    m_sheet_set_style_cache_registry.visit_edges(visitor);

    visitor.visit(m_active_view_transition);
    visitor.visit(m_dynamic_view_transition_style_sheet);

    for (auto& view_transition : m_update_callback_queue)
        visitor.visit(view_transition);

    visitor.visit(m_top_layer_elements);
    visitor.visit(m_top_layer_pending_removals);
    visitor.visit(m_elements_with_pending_top_layer_membership_change);
    visitor.visit(m_showing_auto_popover_list);
    visitor.visit(m_showing_hint_popover_list);
    visitor.visit(m_popover_pointerdown_target);
    visitor.visit(m_open_dialogs_list);
    visitor.visit(m_dialog_pointerdown_target);
    visitor.visit(m_console_client);
    visitor.visit(m_previously_repainted_cursor_position);
    if (m_hit_test_display_list)
        m_hit_test_display_list->visit_edges(visitor);
    visitor.visit(m_editing_host_manager);
    visitor.visit(m_editing_history);
    visitor.visit(m_local_storage_holder);
    visitor.visit(m_session_storage_holder);
    visitor.visit(m_render_blocking_elements);
    visitor.visit(m_policy_container);
    visitor.visit(m_deferred_parser_start);
    visitor.visit(m_custom_element_registry);
    visitor.visit(m_ancestor_origins_list);
}

Utf16String const& Document::content_blocker_style_sheet()
{
    if (is_decoded_svg()) {
        if (!m_content_blocker_style_sheet.has_value())
            m_content_blocker_style_sheet = Utf16String {};
        return m_content_blocker_style_sheet.value();
    }

    if (!m_content_blocker_style_sheet.has_value()) {
        m_content_blocker_style_sheet_checked_classes.clear();
        m_content_blocker_style_sheet_checked_ids.clear();

        Vector<Utf16FlyString> classes;
        Vector<Utf16FlyString> ids;
        for_each_shadow_including_descendant([&](DOM::Node& node) {
            auto* element = as_if<DOM::Element>(node);
            if (!element)
                return TraversalDecision::Continue;

            if (auto const& id = element->id(); id.has_value()) {
                if (!id->is_empty() && m_content_blocker_style_sheet_checked_ids.set(*id) == AK::HashSetResult::InsertedNewEntry)
                    ids.append(*id);
            }

            for (auto const& class_name : element->class_names()) {
                if (!class_name.is_empty() && m_content_blocker_style_sheet_checked_classes.set(class_name) == AK::HashSetResult::InsertedNewEntry)
                    classes.append(class_name);
            }

            return TraversalDecision::Continue;
        });

        m_content_blocker_style_sheet = ContentBlocker::the().cosmetic_style_sheet_for_url(fallback_base_url(), classes, ids);
    }

    return m_content_blocker_style_sheet.value();
}

void Document::invalidate_content_blocker_style_sheet()
{
    m_content_blocker_style_sheet.clear();
    m_content_blocker_style_sheet_checked_classes.clear();
    m_content_blocker_style_sheet_checked_ids.clear();
}

bool Document::content_blocker_style_sheet_may_need_refresh_for_class_or_id(Utf16FlyString const* id, ReadonlySpan<Utf16FlyString> class_names)
{
    if (is_decoded_svg())
        return false;

    if (!m_content_blocker_style_sheet.has_value())
        return false;

    Vector<Utf16FlyString> classes_to_check;
    Vector<Utf16FlyString> ids_to_check;
    auto append_new_token = [](Utf16FlyString const& token, HashTable<Utf16FlyString>& checked_tokens, Vector<Utf16FlyString>& tokens_to_check) {
        if (!token.is_empty() && checked_tokens.set(token) == AK::HashSetResult::InsertedNewEntry)
            tokens_to_check.append(token);
    };

    if (id)
        append_new_token(*id, m_content_blocker_style_sheet_checked_ids, ids_to_check);

    for (auto const& class_name : class_names)
        append_new_token(class_name, m_content_blocker_style_sheet_checked_classes, classes_to_check);

    if (classes_to_check.is_empty() && ids_to_check.is_empty())
        return false;

    return ContentBlocker::the().has_generic_cosmetic_selectors_for_url(fallback_base_url(), classes_to_check, ids_to_check);
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
WebIDL::ExceptionOr<void> Document::write(Utf16View text)
{
    // The document.write(...text) method steps are to run the document write steps with this, text, false, and "Document write".
    return run_the_document_write_steps(text, AddLineFeed::No);
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-document-writeln
WebIDL::ExceptionOr<void> Document::writeln(Utf16View text)
{
    // The document.writeln(...text) method steps are to run the document write steps with this, text, true, and "Document writeln".
    return run_the_document_write_steps(text, AddLineFeed::Yes);
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#document-write-steps
WebIDL::ExceptionOr<void> Document::run_the_document_write_steps(Utf16View text, AddLineFeed line_feed)
{
    // 1-4. The binding layer concatenates text and validates it with Trusted Types.
    Utf16StringBuilder string;
    string.append(text);

    // 5. If lineFeed is true, append U+000A LINE FEED to string.
    if (line_feed == AddLineFeed::Yes)
        string.append_ascii('\n');

    // 6. If document is an XML document, then throw an "InvalidStateError" DOMException.
    if (m_type == Type::XML)
        return WebIDL::InvalidStateError::create("write() called on XML document."_utf16);

    // 7. If document's throw-on-dynamic-markup-insertion counter is greater than 0, then throw an "InvalidStateError" DOMException.
    if (m_throw_on_dynamic_markup_insertion_counter > 0)
        return WebIDL::InvalidStateError::create("throw-on-dynamic-markup-insertion-counter greater than zero."_utf16);

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
    m_parser->tokenizer().insert_input_at_insertion_point(string.view());

    // 11. If document's pending parsing-blocking script is null, then have the HTML parser process string, one code
    //     point at a time, processing resulting tokens as they are emitted, and stopping when the tokenizer reaches
    //     the insertion point or when the processing of the tokenizer is aborted by the tree construction stage (this
    //     can happen if a script end tag token is emitted by the tokenizer).
    if (!has_pending_parsing_blocking_script())
        m_parser->run(HTML::HTMLTokenizer::StopAtInsertionPoint::Yes);

    return {};
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-document-open
WebIDL::ExceptionOr<Document*> Document::open(Optional<Utf16String> const&, Optional<Utf16String> const&)
{
    // 1. If document is an XML document, then throw an "InvalidStateError" DOMException.
    if (m_type == Type::XML)
        return WebIDL::InvalidStateError::create("open() called on XML document."_utf16);

    // 2. If document's throw-on-dynamic-markup-insertion counter is greater than 0, then throw an "InvalidStateError" DOMException.
    if (m_throw_on_dynamic_markup_insertion_counter > 0)
        return WebIDL::InvalidStateError::create("throw-on-dynamic-markup-insertion-counter greater than zero."_utf16);

    // 3. Let entryDocument be the entry global object's associated Document.
    auto* entry_window = HTML::window_from_global_object(HTML::entry_global_object());
    VERIFY(entry_window);
    auto& entry_document = entry_window->associated_document();

    // 4. If document's origin is not same origin to entryDocument's origin, then throw a "SecurityError" DOMException.
    if (origin() != entry_document.origin())
        return WebIDL::SecurityError::create("Document.origin() not the same as entryDocument's."_utf16);

    // 5. If document has an active parser whose script nesting level is greater than 0, then return document.
    if (m_parser && m_parser->script_nesting_level() > 0)
        return this;

    // 6. Similarly, if document's unload counter is greater than 0, then return document.
    if (m_unload_counter > 0)
        return this;

    // 7. If document's active parser was aborted is true, then return document.
    if (m_active_parser_was_aborted)
        return this;

    // 8. If document's node navigable is non-null and document's node navigable's ongoing navigation is a navigation ID, then stop loading document's node navigable.
    // AD-HOC: Pending navigations can also sit in m_pending_navigations, so we need to cancel those too.
    if (auto navigable = this->navigable()) {
        navigable->clear_pending_navigations();
        if (navigable->ongoing_navigation().has<Utf16String>())
            navigable->stop_loading();
    }

    // FIXME: 9. For each shadow-including inclusive descendant node of document, erase all event listeners and handlers given node.

    // FIXME: 10. If document is the associated Document of document's relevant global object, then erase all event listeners and handlers given document's relevant global object.

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

        // 3. Run the URL and history update steps with document and newURL.
        HTML::perform_url_and_history_update_steps(*this, move(new_url));
    }

    // AD-HOC: Record that this document was an initial about:blank before document.open() cleared the flag, so the
    //         Navigation API can keep entries and events disabled.
    if (is_initial_about_blank())
        HTML::relevant_window(*this).navigation()->set_was_initial_about_blank_opened(true);

    // 13. Set document's is initial about:blank to false.
    set_is_initial_about_blank(false);

    // FIXME: 14. If document's iframe load in progress flag is set, then set document's mute iframe load flag.

    // 15. Set document to no-quirks mode.
    set_quirks_mode(QuirksMode::No);

    // INTEROP: The HTML Standard says the document open steps do not affect whether a Document is ready for post-load
    //          tasks. Blink and WebKit reset their corresponding frame load-completion state, while Gecko marks the
    //          document loader as opened but not loaded. Reset our load gating flag for the replacement contents too.
    set_ready_for_post_load_tasks(false);

    // INTEROP: The HTML Standard does not reset the page showing flag in the document open steps. Browsers allow a
    //          completed Document to be reopened and complete loading again, so mark the replaced contents as no longer
    //          showing without firing pagehide or otherwise unloading the Document.
    set_page_showing(false);

    // INTEROP: Cancel completion of the parser that document.open() is about to replace.
    if (m_html_parser_end_state) {
        m_html_parser_end_state->cancel();
        m_html_parser_end_state = nullptr;
    }

    // INTEROP: The HTML Standard leaves discarding pending scripts unspecified. Blink and WebKit discard their
    //          parser script runner's pending parsing-blocking and deferred scripts when replacing the parser, and
    //          Gecko cancels pending parser script loads when terminating the parser. Stop discarded scripts from
    //          delaying the replacement document's load event before removing them from the parser's queues.
    if (m_pending_parsing_blocking_script)
        m_pending_parsing_blocking_script->stop_delaying_document_load_event({});
    if (m_pending_parsing_blocking_svg_script)
        m_pending_parsing_blocking_svg_script->stop_delaying_document_load_event({});
    for (auto& script : m_scripts_to_execute_when_parsing_has_finished)
        script->stop_delaying_document_load_event({});
    m_pending_parsing_blocking_script = nullptr;
    m_pending_parsing_blocking_svg_script = nullptr;
    m_scripts_to_execute_when_parsing_has_finished.clear();

    // 16. Create an HTML parser whose allow declarative shadow roots is document's allow declarative shadow roots, and
    //     associate it with document. This is a script-created parser (meaning that it can be closed by the document.open()
    //     and document.close() methods, and that the tokenizer will wait for an explicit call to document.close() before
    //     emitting an end-of-file token). The encoding confidence is irrelevant.
    m_parser = HTML::HTMLParser::create_for_scripting(*this);
    m_parser->set_allow_declarative_shadow_roots(allow_declarative_shadow_roots());

    // 17. Set the insertion point to point at just before the end of the input stream (which at this point will be empty).
    m_parser->tokenizer().update_insertion_point();

    // 18. Set the parser's allow declarative shadow roots to true.
    m_parser->set_allow_declarative_shadow_roots(HTML::HTMLParser::AllowDeclarativeShadowRoots::Yes);

    // 19. Update the current document readiness of document to "loading".
    update_readiness(HTML::DocumentReadyState::Loading);

    // 20. Return document.
    return this;
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-document-open-window
WebIDL::ExceptionOr<GC::Ptr<HTML::WindowProxy>> Document::open(Utf16View url, Utf16View name, Utf16View features)
{
    // 1. If this is not fully active, then throw an "InvalidAccessError" DOMException.
    if (!is_fully_active())
        return WebIDL::InvalidAccessError::create("Cannot perform open on a document that isn't fully active."_utf16);

    // 2. Return the result of running the window open steps with url, name, and features.
    return window()->window_open_steps(url, name, features);
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#closing-the-input-stream
WebIDL::ExceptionOr<void> Document::close()
{
    // 1. If document is an XML document, then throw an "InvalidStateError" DOMException exception.
    if (m_type == Type::XML)
        return WebIDL::InvalidStateError::create("close() called on XML document."_utf16);

    // 2. If document's throw-on-dynamic-markup-insertion counter is greater than 0, then throw an "InvalidStateError" DOMException.
    if (m_throw_on_dynamic_markup_insertion_counter > 0)
        return WebIDL::InvalidStateError::create("throw-on-dynamic-markup-insertion-counter greater than zero."_utf16);

    // 3. If there is no script-created parser associated with the document, then return.
    if (!m_parser || !m_parser->is_script_created() || m_parser->tokenizer().is_input_stream_closed())
        return {};

    auto parser = m_parser;

    // 4. Insert an explicit "EOF" character at the end of the parser's input stream.
    parser->tokenizer().insert_eof();

    auto finish_script_created_parser = [parser] {
        HTML::HTMLParser::the_end(parser->document(), parser);
    };

    // 5. If there is a pending parsing-blocking script, then return.
    if (has_pending_parsing_blocking_script()) {
        parser->set_post_parse_action(move(finish_script_created_parser));
        return {};
    }

    // 6. Run the tokenizer, processing resulting tokens as they are emitted, and stopping when the tokenizer reaches the explicit "EOF" character or spins the event loop.
    parser->run();

    // INTEROP: Running the tokenizer can invoke author code which calls document.open(), replacing the parser.
    //          Blink, WebKit, and Gecko detach or terminate the old parser in this case, so do not attach its
    //          completion callback to the replacement parser.
    if (m_parser != parser)
        return {};

    // run() may have paused on a blocking script (e.g. from document.write inside an inline script).
    if (has_pending_parsing_blocking_script()) {
        parser->set_post_parse_action(move(finish_script_created_parser));
        return {};
    }

    finish_script_created_parser();

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

bool Document::is_child_allowed(Node const& node) const
{
    switch (node.type()) {
    case NodeType::DOCUMENT_NODE:
    case NodeType::TEXT_NODE:
        return false;
    case NodeType::COMMENT_NODE:
    case NodeType::PROCESSING_INSTRUCTION_NODE:
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

// https://www.w3.org/TR/SVG2/struct.html#InterfaceDocumentExtensions
GC::Ptr<SVG::SVGSVGElement> Document::root_element()
{
    return as_if<SVG::SVGSVGElement>(document_element());
}

// https://html.spec.whatwg.org/multipage/dom.html#the-html-element-2
HTML::HTMLHtmlElement* Document::html_element()
{
    // The html element of a document is its document element, if it's an html element, and null otherwise.
    auto* html = document_element();
    return as_if<HTML::HTMLHtmlElement>(html);
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
Utf16FlyString Document::dir() const
{
    // The dir IDL attribute on Document objects must reflect the dir content attribute of the html
    // element, if any, limited to only known values. If there is no such element, then the
    // attribute must return the empty string and do nothing on setting.
    if (auto html = html_element())
        return html->dir();

    return {};
}

// https://html.spec.whatwg.org/multipage/dom.html#dom-document-dir
void Document::set_dir(Utf16View dir)
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
        return WebIDL::HierarchyRequestError::create("Invalid document body element, must be 'body' or 'frameset'"_utf16);

    auto* existing_body = body();
    if (existing_body) {
        (void)TRY(existing_body->parent()->replace_child(*new_body, *existing_body));
        return {};
    }

    auto* document_element = this->document_element();
    if (!document_element)
        return WebIDL::HierarchyRequestError::create("Missing document element"_utf16);

    (void)TRY(document_element->append_child(*new_body));
    return {};
}

// https://html.spec.whatwg.org/multipage/dom.html#document.title
Utf16String Document::title() const
{
    Utf16String svg_title;
    Optional<Utf16String> html_title;
    Utf16View value = u""sv;

    // 1. If the document element is an SVG svg element, then let value be the child text content of the first SVG title
    //    element that is a child of the document element.
    if (auto const* document_element = this->document_element(); is<SVG::SVGSVGElement>(document_element)) {
        if (auto const* title_element = document_element->first_child_of_type<SVG::SVGTitleElement>()) {
            svg_title = title_element->child_text_content();
            value = svg_title.utf16_view();
        }
    }

    // 2. Otherwise, let value be the child text content of the title element, or the empty string if the title element
    //    is null.
    else if (auto title_element = this->title_element()) {
        html_title = title_element->text_content();
        value = html_title.has_value() ? html_title->utf16_view() : u""sv;
    }

    // 3. Strip and collapse ASCII whitespace in value.
    auto title = Infra::strip_and_collapse_whitespace(value);

    // 4. Return value.
    return title;
}

// https://html.spec.whatwg.org/multipage/dom.html#document.title
WebIDL::ExceptionOr<void> Document::set_title(Utf16View title)
{
    auto* document_element = this->document_element();

    // -> If the document element is an SVG svg element
    if (is<SVG::SVGSVGElement>(document_element)) {
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

void Document::set_layout_root(Layout::Viewport& viewport)
{
    if (m_layout_root.ptr() == &viewport)
        return;
    m_layout_root = viewport;
    m_paint_state = make<Painting::DocumentPaintState>(layout_node_arena());
}

void Document::tear_down_layout_tree()
{
    if (m_layout_root)
        m_layout_root->prepare_subtree_for_detach_from_layout_tree();
    m_hit_test_display_list = nullptr;
    m_chrome_widget_registry->clear();
    m_layout_root = nullptr;
    m_paint_state = nullptr;
    if (m_layout_node_arena)
        Layout::RustFFI::layout_arena_clear_scrollable_overflow_contained_boxes(m_layout_node_arena->handle());
    m_needs_full_layout_tree_update = true;
}

void Document::tear_down_layout_tree_for_svg_image_document(Badge<SVG::SVGDecodedImageData>)
{
    clear_layout_nodes_for_inactive_document();
    tear_down_layout_tree();
}

void Document::clear_layout_nodes_for_inactive_document()
{
    for_each_in_inclusive_subtree([&](auto& node) {
        node.clear_layout_node({});
        if (auto* element = as_if<Element>(node))
            element->clear_synthetic_pseudo_element_layout_nodes(Badge<Document> {});
        return TraversalDecision::Continue;
    });
}

Color Document::background_color() const
{
    // CSS2 says we should use the HTML element's background color unless it's transparent...
    // NB: Called during painting inside update_layout().
    if (auto* html_element = this->html_element(); html_element && html_element->unsafe_layout_node()) {
        auto color = html_element->unsafe_layout_node()->background_color();
        if (color.alpha())
            return color;
    }

    // ...in which case we use the BODY element's background color.
    if (auto* body_element = body(); body_element && body_element->unsafe_layout_node()) {
        auto color = body_element->unsafe_layout_node()->background_color();
        return color;
    }

    // By default, the document is transparent.
    // The outermost canvas is colored by the PageHost.
    return Color::Transparent;
}

Color Document::canvas_background_color() const
{
    return CSS::SystemColor::canvas(canvas_color_scheme()).blend(background_color());
}

CSS::PreferredColorScheme Document::canvas_color_scheme() const
{
    auto color_scheme = CSS::PreferredColorScheme::Light;
    auto root_color_scheme_is_normal = true;
    auto root_color_scheme_was_computed = false;
    if (auto* html_element = this->html_element(); html_element && html_element->layout_node()) {
        root_color_scheme_was_computed = true;
        auto const& layout_node = *html_element->layout_node();
        root_color_scheme_is_normal = layout_node.color_schemes().is_empty();
        if (layout_node.color_scheme() == CSS::PreferredColorScheme::Dark) {
            color_scheme = CSS::PreferredColorScheme::Dark;
        } else if (root_color_scheme_is_normal && m_supported_color_schemes.has_value()) {
            auto preferred_color_scheme = page().preferred_color_scheme();
            if (m_supported_color_schemes->contains_slow(CSS::preferred_color_scheme_to_utf16_fly_string(preferred_color_scheme)))
                color_scheme = preferred_color_scheme;
        }
    }

    if (color_scheme == CSS::PreferredColorScheme::Light
        && !root_color_scheme_was_computed
        && root_color_scheme_is_normal
        && !m_supported_color_schemes.has_value()
        && readiness() == HTML::DocumentReadyState::Loading) {
        if (auto navigable = this->navigable(); navigable && navigable->is_top_level_traversable())
            color_scheme = page().preferred_color_scheme();
    }

    return color_scheme;
}

CSS::ImageRendering Document::background_image_rendering() const
{
    auto* body_element = body();
    if (!body_element)
        return CSS::ImageRendering::Auto;

    // NB: Called during painting inside update_layout().
    auto body_layout_node = body_element->unsafe_layout_node();
    if (!body_layout_node)
        return CSS::ImageRendering::Auto;

    return body_layout_node->image_rendering();
}

void Document::update_base_element(Badge<HTML::HTMLBaseElement>)
{
    GC::Ptr<HTML::HTMLBaseElement> base_element_with_href = nullptr;
    GC::Ptr<HTML::HTMLBaseElement> base_element_with_target = nullptr;

    for_each_in_subtree_of_type<HTML::HTMLBaseElement>([&base_element_with_href, &base_element_with_target](HTML::HTMLBaseElement& base_element_in_tree) {
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

GC::Ptr<HTML::HTMLBaseElement> Document::first_base_element_with_href_in_tree_order() const
{
    return m_first_base_element_with_href_in_tree_order;
}

GC::Ptr<HTML::HTMLBaseElement> Document::first_base_element_with_target_in_tree_order() const
{
    return m_first_base_element_with_target_in_tree_order;
}

// https://html.spec.whatwg.org/multipage/urls-and-fetching.html#respond-to-base-url-changes
void Document::respond_to_base_url_changes(URL::URL const& old_document_url, URL::URL const& old_base_url)
{
    // To respond to base URL changes for a Document document:

    // 1. The user agent should update any user interface elements which are displaying affected URLs, or data derived
    //    from such URLs, to the user. Examples of such user interface elements would be a status bar that displays a
    //    hyperlink's url, or some user interface which displays the URL specified by a q, blockquote, ins, or del
    //    element's cite attribute.
    // FIXME: Update those UI elements.

    // 2. Ensure that the CSS :link/:visited/etc. pseudo-classes are updated appropriately.
    // FIXME: When we track which links have been visited, then :link and :visited will also depend on the new URL and
    //        the link walk below will need to take those pseudo-classes into account.
    auto const& new_document_url = m_url;
    auto new_base_url = base_url();
    bool const base_url_unchanged = (old_base_url == new_base_url);
    if (base_url_unchanged && old_document_url == new_document_url)
        return;

    auto encoding = encoding_or_default();
    auto local_link_match = [&](Optional<URL::URL> const& target_url, URL::URL const& document_url) {
        if (!target_url.has_value())
            return false;
        if (target_url->fragment().has_value())
            return document_url.equals(*target_url, URL::ExcludeFragment::No);
        return document_url.equals(*target_url, URL::ExcludeFragment::Yes);
    };

    for_each_shadow_including_descendant([&](Node& node) {
        auto* element = as_if<Element>(node);
        if (!element || !element->matches_link_pseudo_class())
            return TraversalDecision::Continue;

        auto href = element->get_attribute_value(HTML::AttributeNames::href);
        auto old_target_url = DOMURL::parse(href, old_base_url, encoding.utf16_view());
        auto new_target_url = base_url_unchanged ? old_target_url : DOMURL::parse(href, new_base_url, encoding.utf16_view());

        auto was_local_link = local_link_match(old_target_url, old_document_url);
        auto is_local_link = local_link_match(new_target_url, new_document_url);
        if (was_local_link != is_local_link)
            CSS::record_element_state_changed(*element, CSS::PseudoClass::LocalLink, is_local_link);

        return TraversalDecision::Continue;
    });

    // FIXME: 3. For each descendant of document's shadow-including descendants:
    //        ...

    // FIXME: 4. Consider speculative loads given document.
}

// https://html.spec.whatwg.org/multipage/urls-and-fetching.html#set-the-url
void Document::set_url(URL::URL const& url)
{
    // OPTIMIZATION: Avoid unnecessary work if the URL is already set.
    if (m_url == url)
        return;

    ensure_cookie_version_index(url, m_url);

    auto old_document_url = m_url;
    auto old_base_url = base_url();

    // To set the URL for a Document document to a URL record url:

    // 1. Set document's URL to url.
    m_url = url;

    // 2. Respond to base URL changes given document.
    respond_to_base_url_changes(old_document_url, old_base_url);
}

// https://html.spec.whatwg.org/multipage/urls-and-fetching.html#fallback-base-url
URL::URL Document::fallback_base_url() const
{
    // 1. If document is an iframe srcdoc document, then:
    if (HTML::url_matches_about_srcdoc(m_url)) {
        // 1. Assert: document's about base URL is non-null.
        // AD-HOC: Documents created by DOMParser or by cloning can have a URL that matches about:srcdoc
        //         without an about base URL being set. Returning the document's URL in this case matches
        //         the behavior of other engines.
        if (!m_about_base_url.has_value())
            return m_url;

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
    // 1. If document has no descendant base element that has an href attribute, then return document's fallback base URL.
    auto base_element = first_base_element_with_href_in_tree_order();
    if (!base_element)
        return fallback_base_url();

    // 2. Otherwise, return the frozen base URL of the first base element in document that has an href attribute, in tree order.
    return base_element->frozen_base_url();
}

// https://html.spec.whatwg.org/multipage/urls-and-fetching.html#encoding-parsing-a-url
Optional<URL::URL> Document::encoding_parse_url(Utf16View url) const
{
    // 1. Let encoding be UTF-8.
    // 2. If environment is a Document object, then set encoding to environment's character encoding.
    auto encoding = encoding_or_default();

    // 3. Otherwise, if environment's relevant global object is a Window object, set encoding to environment's relevant
    //    global object's associated Document's character encoding.

    // 4. Let baseURL be environment's base URL, if environment is a Document object; otherwise environment's API base URL.
    auto base_url = this->base_url();

    // 5. Return the result of applying the URL parser to url, with baseURL and encoding.
    return DOMURL::parse(url, base_url, encoding.utf16_view());
}

// https://html.spec.whatwg.org/multipage/urls-and-fetching.html#encoding-parsing-and-serializing-a-url
Optional<Utf16String> Document::encoding_parse_and_serialize_url(Utf16View url) const
{
    // 1. Let url be the result of encoding-parsing a URL given url, relative to environment.
    auto parsed_url = encoding_parse_url(url);

    // 2. If url is failure, then return failure.
    if (!parsed_url.has_value())
        return {};

    // 3. Return the result of applying the URL serializer to url.
    return utf16_string_from_url_ascii(parsed_url->serialize());
}

void Document::invalidate_layout_tree(InvalidateLayoutTreeReason reason)
{
    if (m_layout_root)
        dbgln_if(UPDATE_LAYOUT_DEBUG, "DROP TREE {}", to_string(reason));
    tear_down_layout_tree();
}

void Document::PartialRelayoutInvalidation::record_escape(PartialRelayoutEscapeReason reason)
{
    dbgln_if(UPDATE_LAYOUT_DEBUG, "Pending updates escape partial relayout boundaries ({})", to_string(reason));
    m_escapes = true;
}

void Document::PartialRelayoutInvalidation::clear_escape(PartialRelayoutEscapeClearReason reason)
{
    if (m_escapes)
        dbgln_if(UPDATE_LAYOUT_DEBUG, "Pending updates no longer escape partial relayout boundaries ({})", to_string(reason));
    m_escapes = false;
}

// Anchor names publish geometry that anchor() functions on positioned boxes anywhere in the
// document consume, and only a full layout pass re-resolves all of them, so anchor positioning
// in use takes updates off the partial relayout path entirely.
bool Document::any_anchor_names_are_registered() const
{
    if (m_anchor_name_map.has_registered_names())
        return true;
    for (auto const& shadow_root : m_shadow_roots) {
        if (shadow_root.anchor_name_map().has_registered_names())
            return true;
    }
    return false;
}

void Document::set_needs_container_query_evaluation_after_layout(Element const& query_container)
{
    m_query_containers_needing_container_query_evaluation_after_layout.set(const_cast<Element&>(query_container));
}

void Document::begin_style_stabilization_epoch()
{
    if (m_style_stabilization_epoch_depth++ != 0)
        return;

    VERIFY(m_provisional_animation_event_queue.is_empty());
    VERIFY(m_animations_created_in_stabilization_epoch.is_empty());
    style_computer().begin_transition_stabilization_epoch();
    m_style_stabilization_pass_count = 0;
    m_style_stabilization_has_style_reactions = false;
    ++m_style_invalidation_counters.style_stabilization_epochs;
    ++m_transition_generation;
}

void Document::record_style_stabilization_pass()
{
    VERIFY(m_style_stabilization_epoch_depth > 0);
    ++m_style_invalidation_counters.provisional_style_passes;
    ++m_style_stabilization_pass_count;
    if (m_style_stabilization_pass_count == 2)
        ++m_style_invalidation_counters.style_stabilization_feedback_epochs;

    constexpr u64 ordinary_stabilization_round_limit = 8;
    if (m_style_stabilization_pass_count == ordinary_stabilization_round_limit + 1)
        ++m_style_invalidation_counters.style_stabilization_round_guard_hits;
    if (m_style_stabilization_pass_count > ordinary_stabilization_round_limit)
        ++m_style_invalidation_counters.exact_stabilization_passes;

    // Size-query and style-reaction dependencies are acyclic, so a coherent pass settles at
    // least one more connected element. Include inner StyleEngine transactions in the same exact
    // bound as layout feedback so neither feedback path can spin independently of the epoch.
    auto const exact_stabilization_round_limit = static_cast<u64>(style_computer().style_engine().connected_element_count()) + 1;
    if (m_style_stabilization_pass_count > ordinary_stabilization_round_limit + exact_stabilization_round_limit) {
        ++m_style_invalidation_counters.style_stabilization_bound_failures;
        VERIFY_NOT_REACHED();
    }
}

void Document::end_style_stabilization_epoch()
{
    VERIFY(m_style_stabilization_epoch_depth > 0);
    if (m_style_stabilization_epoch_depth > 1) {
        --m_style_stabilization_epoch_depth;
        return;
    }

    // Transition actions are committed while the epoch is still active so cancellation events join
    // the provisional event queue and are published by the same observation barrier below.
    style_computer().commit_transition_stabilization_epoch();
    --m_style_stabilization_epoch_depth;

    for (auto const& event : m_provisional_animation_event_queue) {
        // An animation created and cancelled inside this epoch was never part of an externally
        // committed style. Its cancellation is provisional too, so no observer may receive it.
        if (m_animations_created_in_stabilization_epoch.contains(event.animation) && event.animation->is_idle())
            continue;
        m_pending_animation_event_queue.append(event);
        ++m_style_invalidation_counters.committed_animation_events;
    }
    m_provisional_animation_event_queue.clear();
    m_animations_created_in_stabilization_epoch.clear();
}

static void relayout_subtree(Layout::Box& subtree_root)
{
    Layout::LayoutRustBridge bridge;
    // Absolutely positioned boundaries re-resolve their own size and position by replaying
    // their layout from saved inputs; SVG root boundaries keep the frozen geometry saved at
    // the previous commit. The commit sink resolves the committed row to splice out in either
    // path.
    if (subtree_root.is_absolutely_positioned()) {
        VERIFY(subtree_root.containing_block());
        VERIFY(subtree_root.has_saved_abspos_layout_inputs());
        bridge.replay_saved_abspos_layout(subtree_root);
    } else {
        bridge.compute_subtree_layout(subtree_root);
    }

    Layout::RustFFI::layout_arena_reset_layout_update_flags_in_subtree(
        subtree_root.arena_handle(), Layout::Node::slot_id(&subtree_root));
}

// Recomputes containing blocks and derives the abspos escape flags for the inclusive subtree
// inside the Rust arena; the DOM-ancestry half of the inline containing-block workaround stays
// on the C++ side as the callback.
static void recompute_containing_blocks_in_inclusive_subtree(Layout::NodeArena& arena, Layout::Node& subtree_root)
{
    Layout::RustFFI::layout_arena_recompute_containing_blocks(
        arena.handle(), Layout::Node::slot_id(&subtree_root),
        [](void* node_shell, void* containing_block_shell) -> Layout::RustFFI::NodeSlotId {
            auto const& node = *static_cast<Layout::Node const*>(node_shell);
            auto const& containing_block = *static_cast<Layout::Box const*>(containing_block_shell);
            return Layout::Node::slot_id(node.find_inline_containing_block(containing_block));
        });
}

// Refreshes every structure derived from committed layout results, shared by the partial and
// full layout paths so neither can forget one.
void Document::after_layout_commit(LayoutTreeChanged layout_tree_changed, LayoutCommitScope layout_commit_scope)
{
    // NB: Called during layout update.
    m_layout_root->invalidate_text_blocks_cache();

    set_needs_to_record_display_list();
    set_needs_to_refresh_scroll_state(true);

    // A commit that changed the tree can have replaced boxes referenced by the cached
    // contained-boxes index; refresh it before overflow measurement follows them. A pending full
    // recalculation rebuilds the index inside its own measurement traversal instead.
    if (layout_tree_changed == LayoutTreeChanged::Yes && !Layout::RustFFI::layout_arena_needs_full_scrollable_overflow_recalculation(layout_node_arena().handle()))
        Layout::RustFFI::layout_arena_rebuild_scrollable_overflow_contained_boxes(layout_node_arena().handle(), Layout::Node::slot_id(m_layout_root.ptr()));
    if (layout_commit_scope == LayoutCommitScope::Full)
        update_scrollable_overflow(ScrollableOverflowDerivedStructureUpdates::HandledByFullLayoutCommit);
    else
        update_scrollable_overflow(ScrollableOverflowDerivedStructureUpdates::HandledByAfterLayoutCommit);

    set_needs_accumulated_visual_contexts_update(true);

    // Selection state lives on committed fragments, which the commit has rebuilt.
    if (auto range = get_selection()->range())
        paint_state().recompute_selection_states(*this, *range);

    if (layout_tree_changed == LayoutTreeChanged::Yes) {
        // Broadcast the current viewport rect to any new committed boxes, so they know whether
        // they're visible or not. If necessary, re-collect the content-visibility:auto set.
        inform_all_viewport_clients_about_the_current_viewport_rect();
        if (m_may_have_content_visibility_auto_style)
            collect_boxes_with_auto_content_visibility();
    }

    schedule_scroll_container_resnap();

    m_document->set_needs_repaint();
}

static void propagate_scrollbar_width_to_viewport(Element& root_element, Layout::Viewport& viewport)
{
    // https://drafts.csswg.org/css-scrollbars/#scrollbar-width
    // UAs must apply the scrollbar-color value set on the root element to the viewport.
    // NB: Called during layout tree construction.
    auto scrollbar_width = root_element.unsafe_layout_node()->scrollbar_width();
    viewport.modify_computed_values([&](auto& values) {
        values.set_scrollbar_width(scrollbar_width);
    });
}

// https://drafts.csswg.org/css-writing-modes-4/#principal-flow
static void propagate_principal_writing_mode_to_viewport(Element& root_element, Layout::Viewport& viewport)
{
    // The principal writing mode of the document is determined by the used writing-mode, direction, and
    // text-orientation values of the root element.
    auto const* root_inherited_box_values = root_element.style_group<CSS::ComputedValues::InheritedBoxValues>();
    VERIFY(root_inherited_box_values);
    auto writing_mode = static_cast<CSS::WritingMode>(root_inherited_box_values->writing_mode);
    auto direction = static_cast<CSS::Direction>(root_inherited_box_values->direction);

    // As a special case for handling HTML documents, if the root element has a body child element [HTML] whose
    // display value is not none, the used value of the of writing-mode and direction properties on root element
    // are taken from the computed writing-mode and direction of the first such child element instead of from the
    // root element's own values.
    // NOTE: Using containment disables this special handling of the HTML body element.
    auto* body_element = root_element.first_child_of_type<HTML::HTMLBodyElement>();
    auto const* root_box_values = root_element.style_group<CSS::ComputedValues::BoxValues>();
    VERIFY(root_box_values);
    auto const* body_box_values = body_element ? body_element->style_group<CSS::ComputedValues::BoxValues>() : nullptr;
    auto const* body_inherited_box_values = body_element ? body_element->style_group<CSS::ComputedValues::InheritedBoxValues>() : nullptr;
    auto has_containment = [](CSS::ComputedValues::BoxValues const& values) {
        return values.size_containment || values.inline_size_containment || values.layout_containment || values.style_containment || values.paint_containment;
    };
    bool propagation_is_disabled_by_containment = has_containment(*root_box_values)
        || (body_box_values && has_containment(*body_box_values));
    if (root_element.is_html_html_element() && !propagation_is_disabled_by_containment
        && body_box_values && body_inherited_box_values && !CSS::display_from_ffi_display(body_box_values->display).is_none()) {
        writing_mode = static_cast<CSS::WritingMode>(body_inherited_box_values->writing_mode);
        direction = static_cast<CSS::Direction>(body_inherited_box_values->direction);
    }
    root_element.unsafe_layout_node()->modify_computed_values([&](auto& values) {
        values.set_writing_mode(writing_mode);
        values.set_direction(direction);
    });

    // https://drafts.csswg.org/css-writing-modes-4/#icb
    // The principal writing mode is propagated to the initial containing block and to the viewport, thereby
    // affecting the layout of the root element and the scrolling direction of the viewport.
    viewport.modify_computed_values([&](auto& values) {
        values.set_writing_mode(writing_mode);
        values.set_direction(direction);
    });
}

// https://drafts.csswg.org/css-overflow-3/#overflow-propagation
static void propagate_overflow_to_viewport(Element& root_element, Layout::Viewport& viewport)
{
    // https://drafts.csswg.org/css-contain-2/#contain-property
    // Additionally, when any containments are active on either the HTML <html> or <body> elements, propagation of
    // properties from the <body> element to the initial containing block, the viewport, or the canvas background, is
    // disabled. Notably, this affects:
    // - 'overflow' and its longhands (see CSS Overflow 3 § 3.3 Overflow Viewport Propagation)
    auto* body_element = root_element.first_child_of_type<HTML::HTMLBodyElement>();
    auto const* root_box_values = root_element.style_group<CSS::ComputedValues::BoxValues>();
    VERIFY(root_box_values);
    auto has_containment = [](CSS::ComputedValues::BoxValues const& values) {
        return values.size_containment || values.inline_size_containment || values.layout_containment || values.style_containment || values.paint_containment;
    };
    auto const* body_box_values = body_element ? body_element->style_group<CSS::ComputedValues::BoxValues>() : nullptr;
    bool body_element_can_propagate_overflow = body_element
        && body_box_values
        && !CSS::display_from_ffi_display(body_box_values->display).is_none()
        && body_element->unsafe_layout_node();
    bool body_propagation_is_disabled_by_containment = root_element.is_html_html_element() && has_containment(*root_box_values);
    if (body_box_values && has_containment(*body_box_values))
        body_propagation_is_disabled_by_containment = true;

    // UAs must apply the overflow-* values set on the root element to the viewport
    // when the root element’s display value is not none.
    auto root_element_layout_node = root_element.unsafe_layout_node();
    auto root_overflow_x = static_cast<CSS::Overflow>(root_box_values->overflow_x);
    auto root_overflow_y = static_cast<CSS::Overflow>(root_box_values->overflow_y);

    Element* overflow_origin_element = &root_element;

    // However, when the root element is an [HTML] html element (including XML syntax for HTML)
    // whose overflow value is visible (in both axes), and that element has as a child
    // a body element whose display value is also not none,
    // user agents must instead apply the overflow-* values of the first such child element to the viewport.
    if (root_element.is_html_html_element() && !body_propagation_is_disabled_by_containment) {
        if (root_overflow_x == CSS::Overflow::Visible && root_overflow_y == CSS::Overflow::Visible) {
            if (body_element_can_propagate_overflow)
                overflow_origin_element = body_element;
        }
    }

    // If 'visible' is applied to the viewport, it must be interpreted as 'auto'. If 'clip' is applied to the viewport, it must be interpreted as 'hidden'.
    auto const* overflow_origin_box_values = overflow_origin_element == &root_element ? root_box_values : body_box_values;
    auto overflow_x_to_apply = static_cast<CSS::Overflow>(overflow_origin_box_values->overflow_x);
    if (overflow_x_to_apply == CSS::Overflow::Visible) {
        overflow_x_to_apply = CSS::Overflow::Auto;
    } else if (overflow_x_to_apply == CSS::Overflow::Clip) {
        overflow_x_to_apply = CSS::Overflow::Hidden;
    }
    auto overflow_y_to_apply = static_cast<CSS::Overflow>(overflow_origin_box_values->overflow_y);
    if (overflow_y_to_apply == CSS::Overflow::Visible) {
        overflow_y_to_apply = CSS::Overflow::Auto;
    } else if (overflow_y_to_apply == CSS::Overflow::Clip) {
        overflow_y_to_apply = CSS::Overflow::Hidden;
    }
    // Every node receives its final values exactly once: a steady-state pass then leaves
    // every style group payload untouched instead of oscillating values within the pass.
    viewport.modify_computed_values([&](auto& values) {
        values.set_overflow_x(overflow_x_to_apply);
        values.set_overflow_y(overflow_y_to_apply);
    });

    // UAs must apply the overflow-* values set on the root element to the viewport
    // when the root element's display value is not none.
    // The element from which the value is propagated must then have a used overflow value of visible.
    // FIXME: Apply this to the used values, not the computed ones.
    bool root_element_is_overflow_origin = overflow_origin_element == &root_element;
    root_element_layout_node->modify_computed_values([&](auto& values) {
        values.set_overflow_x(root_element_is_overflow_origin ? CSS::Overflow::Visible : root_overflow_x);
        values.set_overflow_y(root_element_is_overflow_origin ? CSS::Overflow::Visible : root_overflow_y);
    });
    if (body_element_can_propagate_overflow) {
        bool body_element_is_overflow_origin = overflow_origin_element == body_element;
        auto body_overflow_x = static_cast<CSS::Overflow>(body_box_values->overflow_x);
        auto body_overflow_y = static_cast<CSS::Overflow>(body_box_values->overflow_y);
        body_element->unsafe_layout_node()->modify_computed_values([&](auto& values) {
            values.set_overflow_x(body_element_is_overflow_origin ? CSS::Overflow::Visible : body_overflow_x);
            values.set_overflow_y(body_element_is_overflow_origin ? CSS::Overflow::Visible : body_overflow_y);
        });
    }
}

void Document::update_layout_if_needed_for_node(Node const& node, UpdateLayoutReason reason)
{
    if (!node.is_connected())
        return;

    if (reason != UpdateLayoutReason::HTMLEventLoopRenderingUpdate)
        flush_throttled_animation_style_update_for_node(node);

    auto* document_element = this->document_element();
    auto const may_have_style_query_dependencies = document_element && document_element->is_style_query_container();
    auto const reads_layout_geometry = reason == UpdateLayoutReason::ElementGetClientRects
        || reason == UpdateLayoutReason::ElementClientWidth
        || reason == UpdateLayoutReason::ElementClientHeight
        || reason == UpdateLayoutReason::HTMLElementOffsetWidth
        || reason == UpdateLayoutReason::HTMLElementOffsetHeight;
    if (reads_layout_geometry
        && m_has_completed_style_update
        && layout_is_up_to_date()
        && !m_needs_media_rule_evaluation
        && !m_needs_animated_style_update
        && m_query_containers_needing_container_query_evaluation_after_layout.is_empty()
        && m_elements_with_pending_top_layer_membership_change.is_empty()
        && !m_top_layer_needs_layout_zone_rebuild
        && !style_computer().style_engine().css_transitions_may_observe_style_changes()
        && !may_have_style_query_dependencies) {
        auto document_is_clean_for_layout_geometry_read = [](Document const& document) {
            return document.m_has_completed_style_update
                && document.layout_is_up_to_date()
                && !document.m_has_dirty_style_attributes
                && !document.style_computer().style_engine().has_pending_transaction()
                && !document.m_needs_media_rule_evaluation
                && !document.m_needs_animated_style_update
                && document.m_query_containers_needing_container_query_evaluation_after_layout.is_empty()
                && document.m_elements_with_pending_top_layer_membership_change.is_empty()
                && !document.m_top_layer_needs_layout_zone_rebuild;
        };
        auto embedding_document_chain_is_clean = [&] {
            auto const* embedded_document = this;
            while (auto navigable = embedded_document->navigable()) {
                auto embedding_document = navigable->container_document();
                if (!embedding_document || embedding_document.ptr() == embedded_document)
                    return true;
                if (!document_is_clean_for_layout_geometry_read(*embedding_document))
                    return false;
                embedded_document = embedding_document.ptr();
            }
            return true;
        };
        if (embedding_document_chain_is_clean()) {
            synchronize_dirty_style_attributes();
            if (!style_computer().style_engine().pending_transaction_may_affect_layout_geometry()) {
                // A later inline transition declaration still needs the pending style as its
                // before-change style, even though this geometry read can reuse the current layout.
                if (!style_computer().style_engine().has_pending_transaction()
                    || style_computer().style_engine().defer_pending_transaction_for_geometry_read()) {
                    return;
                }
            }
        }
    }

    update_layout(reason, ThrottledAnimationSamplingScope::Element);
}

void Document::flush_deferred_style_change_event()
{
    auto& style_engine = style_computer().style_engine();
    if (!style_engine.has_deferred_geometry_transaction())
        return;

    if (!style_engine.begin_deferred_geometry_transaction_flush()) {
        // All non-replayable style inputs consume the boundary before changing their authoritative
        // state. Reaching this fallback means exact local-fact journalling coarsened, so preserve
        // correctness by settling the combined transaction even though it cannot remain a distinct
        // transition style-change event.
        update_style();
        return;
    }
    ScopeGuard restore_later_style_inputs = [&] {
        style_engine.end_deferred_geometry_transaction_flush();
    };
    update_style();
}

void Document::schedule_list_item_renumber(Element& list_owner)
{
    m_list_owners_pending_item_renumber.set(list_owner);
}

void Document::process_pending_list_item_renumbers()
{
    if (m_list_owners_pending_item_renumber.is_empty())
        return;
    auto pending = move(m_list_owners_pending_item_renumber);
    for (auto const& list_owner : pending) {
        if (!list_owner->is_connected()) {
            m_list_owners_with_stale_item_counters.remove(list_owner);
            continue;
        }
        if (list_owner->list_item_renumber_affects_rendered_content()) {
            list_owner->set_needs_layout_tree_update(true, SetNeedsLayoutTreeUpdateReason::ListItemCounters);
            m_list_owners_with_stale_item_counters.remove(list_owner);
        } else {
            m_list_owners_with_stale_item_counters.set(list_owner);
        }
    }
}

void Document::did_render_list_item_counter_value(Element& element)
{
    if (m_stale_list_item_counter_rendered || m_list_owners_with_stale_item_counters.is_empty())
        return;
    for (GC::Ptr<Element> ancestor = element; ancestor; ancestor = ancestor->parent_element()) {
        if (m_list_owners_with_stale_item_counters.contains(*ancestor)) {
            m_stale_list_item_counter_rendered = true;
            return;
        }
    }
}

bool Document::reconcile_stale_list_item_counters_after_tree_build(Vector<Layout::Node*> const& rebuilt_subtree_roots)
{
    if (m_list_owners_with_stale_item_counters.is_empty()) {
        m_stale_list_item_counter_rendered = false;
        return false;
    }

    // A rebuilt subtree has re-resolved the counters sets of any stale owner inside it, and an owner that has left
    // the document renders nothing.
    HashTable<Node const*> rebuilt_dom_roots;
    for (auto const* rebuilt_root : rebuilt_subtree_roots) {
        if (auto const* dom_node = rebuilt_root->dom_node())
            rebuilt_dom_roots.set(dom_node);
    }
    m_list_owners_with_stale_item_counters.remove_all_matching([&](GC::Ref<Element> const& list_owner) {
        if (!list_owner->is_connected())
            return true;
        for (Node const* node = list_owner.ptr(); node; node = node->parent()) {
            if (rebuilt_dom_roots.contains(node))
                return true;
        }
        return false;
    });

    if (!m_stale_list_item_counter_rendered)
        return false;
    m_stale_list_item_counter_rendered = false;
    if (m_list_owners_with_stale_item_counters.is_empty())
        return false;
    for (auto const& list_owner : m_list_owners_with_stale_item_counters)
        list_owner->set_needs_layout_tree_update(true, SetNeedsLayoutTreeUpdateReason::ListItemCounters);
    m_list_owners_with_stale_item_counters.clear();
    return true;
}

bool Document::needs_style_update_after_layout()
{
    return !m_query_containers_needing_container_query_evaluation_after_layout.is_empty()
        || m_needs_animated_style_update
        || style_computer().style_engine().has_recorded_input();
}

// Attempts to satisfy the pending layout update by re-laying out only the registered partial
// relayout boundary subtrees. Runs the incremental layout tree build itself when tree updates
// are pending (consuming `needs_layout_tree_rebuild`), so an ineligible update continues to
// the full layout path without rebuilding again.
Document::PartialRelayoutResult Document::try_partial_relayout(Vector<Layout::RustFFI::NodeSlotId> registered_partial_relayout_root_slots, bool& needs_layout_tree_rebuild, bool should_collect_devtools_layout_data)
{
    if (!m_layout_root
        || needs_full_layout_tree_update()
        || m_partial_relayout_invalidation.escapes()
        || registered_partial_relayout_root_slots.is_empty()
        || m_layout_root->needs_layout_update()
        || !m_query_containers_needing_container_query_evaluation_after_layout.is_empty()
        || should_collect_devtools_layout_data
        || any_anchor_names_are_registered())
        return PartialRelayoutResult::NotEligible;

    bool layout_tree_was_built_in_partial_branch = false;
    bool pending_updates_escaped_during_partial_build = false;
    Vector<Layout::Node*> rebuilt_subtree_roots;
    if (needs_layout_tree_rebuild) {
        auto tree_build_timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);
        auto tree_build_result = Layout::build_layout_tree(*this);
        set_layout_root(as<Layout::Viewport>(*tree_build_result.root));
        record_layout_tree_build(tree_build_result.rebuilt_subtree_roots.size(), tree_build_result.layout_tree_update_escaped_rebuild_roots);
        needs_layout_tree_rebuild = false;
        if (reconcile_stale_list_item_counters_after_tree_build(tree_build_result.rebuilt_subtree_roots) || tree_build_result.needs_another_build_pass)
            return PartialRelayoutResult::NeedsAnotherLayoutPass;
        layout_tree_was_built_in_partial_branch = true;
        pending_updates_escaped_during_partial_build = m_partial_relayout_invalidation.escapes()
            || tree_build_result.layout_tree_update_escaped_rebuild_roots;
        m_partial_relayout_invalidation.clear_escape(PartialRelayoutEscapeClearReason::PartialLayoutTreeBuild);
        rebuilt_subtree_roots = move(tree_build_result.rebuilt_subtree_roots);

        if constexpr (UPDATE_LAYOUT_DEBUG) {
            dbgln("TREEBUILD {} µs", tree_build_timer.elapsed_time().to_microseconds());
        }
    }

    if (m_layout_root->needs_layout_update() || pending_updates_escaped_during_partial_build)
        return PartialRelayoutResult::NotEligible;

    // Nodes created by the incremental build have no containing blocks assigned yet, and the
    // mutation may have moved where existing out-of-flow descendants belong; recompute both so
    // boundary qualification below reads facts matching the just-built tree.
    for (auto* rebuilt_root : rebuilt_subtree_roots)
        recompute_containing_blocks_in_inclusive_subtree(layout_node_arena(), *rebuilt_root);

    Vector<Layout::RustFFI::NodeSlotId> rebuilt_subtree_root_slots;
    rebuilt_subtree_root_slots.ensure_capacity(rebuilt_subtree_roots.size());
    for (auto* rebuilt_root : rebuilt_subtree_roots)
        rebuilt_subtree_root_slots.unchecked_append(Layout::Node::slot_id(rebuilt_root));

    Vector<Layout::RustFFI::NodeSlotId> partial_relayout_root_slots;
    bool boundary_set_supports_partial_relayout = Layout::RustFFI::layout_arena_collect_partial_relayout_roots(
        layout_node_arena().handle(),
        registered_partial_relayout_root_slots.data(), registered_partial_relayout_root_slots.size(),
        rebuilt_subtree_root_slots.data(), rebuilt_subtree_root_slots.size(),
        &partial_relayout_root_slots,
        [](void* context, Layout::RustFFI::NodeSlotId root) {
            static_cast<Vector<Layout::RustFFI::NodeSlotId>*>(context)->append(root);
        });
    if (!boundary_set_supports_partial_relayout)
        return PartialRelayoutResult::NotEligible;

    Vector<Layout::Box*> partial_relayout_roots;
    partial_relayout_roots.ensure_capacity(partial_relayout_root_slots.size());
    for (auto slot : partial_relayout_root_slots) {
        auto* root = static_cast<Layout::Node*>(Layout::RustFFI::layout_arena_node_shell_if_live(layout_node_arena().handle(), slot));
        partial_relayout_roots.unchecked_append(&as<Layout::Box>(*root));
    }

    // The final boundary can be wider than the rebuilt roots that led us to it. Replaying such a
    // boundary may re-enter intrinsic sizing for descendants outside those rebuilt roots, including
    // anonymous layout nodes created during the incremental tree build, so refresh the metadata for
    // the whole subtree that is about to be laid out.
    for (auto* root : partial_relayout_roots)
        recompute_containing_blocks_in_inclusive_subtree(layout_node_arena(), *root);

    layout_node_arena().sync_enrolled_content_for_layout();
    for (auto* root : partial_relayout_roots) {
        relayout_subtree(*root);
        // NB: The subtree commit reset the root's descendant rows, and the subtree's
        //     new size may change ancestor scrollable overflow; scheduling the root covers both.
        schedule_scrollable_overflow_recalculation(*root);
    }

    ++m_partial_layout_count;

    after_layout_commit(layout_tree_was_built_in_partial_branch ? LayoutTreeChanged::Yes : LayoutTreeChanged::No, LayoutCommitScope::Subtree);
    if (needs_style_update_after_layout() || !layout_is_up_to_date())
        return PartialRelayoutResult::NeedsAnotherLayoutPass;
    return PartialRelayoutResult::Done;
}

void Document::update_layout(UpdateLayoutReason reason)
{
    update_layout(reason, ThrottledAnimationSamplingScope::Document);
}

void Document::update_layout(UpdateLayoutReason reason, ThrottledAnimationSamplingScope animation_sampling_scope)
{
    auto navigable = this->navigable();
    if (!navigable || navigable->active_document().ptr() != this)
        return;

    if (reason != UpdateLayoutReason::HTMLEventLoopRenderingUpdate && animation_sampling_scope == ThrottledAnimationSamplingScope::Document)
        flush_throttled_animation_style_update();

    VERIFY(!m_is_running_update_layout);
    m_is_running_update_layout = true;
    ScopeGuard guard = [&] {
        m_is_running_update_layout = false;

        if (m_needs_scroll_container_resnap) {
            if (auto navigable = this->navigable(); navigable && navigable->active_document().ptr() == this)
                navigable->re_snap_scroll_containers_after_layout_change();
        }

        page().client().flush_pending_dom_mutations();
    };

    begin_style_stabilization_epoch();
    ScopeGuard end_stabilization_epoch = [&] {
        end_style_stabilization_epoch();
    };

    constexpr u64 ordinary_stabilization_round_limit = 8;
    // Size-query dependencies point from a descendant to an ancestor query container. They are
    // therefore acyclic, and a coherent style/layout pass can settle at least one more level of
    // a nested dependency chain. One pass per connected element is a conservative exact bound.
    // Recompute it after each pass because an initial style update can enroll the elements of a
    // freshly parsed document after update_layout() has already started.
    for (u64 layout_pass = 0; layout_pass < ordinary_stabilization_round_limit + static_cast<u64>(style_computer().style_engine().connected_element_count()) + 1; ++layout_pass) {
        update_style();
        process_pending_list_item_renumbers();
        process_pending_top_layer_layout_changes();

        auto const should_collect_devtools_layout_data = page().client().has_active_devtools_client();
        auto const force_devtools_layout_data_collection = should_collect_devtools_layout_data
            && reason == UpdateLayoutReason::InspectDevToolsLayoutData;

        if (layout_is_up_to_date() && !force_devtools_layout_data_collection) {
            update_scrollable_overflow(ScrollableOverflowDerivedStructureUpdates::UpdateAfterMeasure);
            return;
        }

        Vector<Layout::RustFFI::NodeSlotId> registered_partial_relayout_root_slots;
        Layout::RustFFI::layout_arena_take_partial_relayout_boundary_roots(
            layout_node_arena().handle(), &registered_partial_relayout_root_slots,
            [](void* context, Layout::RustFFI::NodeSlotId slot) {
                static_cast<Vector<Layout::RustFFI::NodeSlotId>*>(context)->append(slot);
            });

        // NOTE: If this is a document hosting <template> contents, layout is unnecessary.
        if (m_created_for_appropriate_template_contents)
            return;

        auto needs_layout_tree_rebuild = !m_layout_root || needs_layout_tree_update() || child_needs_layout_tree_update() || needs_full_layout_tree_update();

        switch (try_partial_relayout(move(registered_partial_relayout_root_slots), needs_layout_tree_rebuild, should_collect_devtools_layout_data)) {
        case PartialRelayoutResult::Done:
            return;
        case PartialRelayoutResult::NeedsAnotherLayoutPass:
            continue;
        case PartialRelayoutResult::NotEligible:
            break;
        }

        auto* document_element = this->document_element();
        auto viewport_rect = navigable->viewport_rect();

        auto timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);

        if (needs_layout_tree_rebuild) {
            auto tree_build_result = Layout::build_layout_tree(*this);
            set_layout_root(as<Layout::Viewport>(*tree_build_result.root));
            record_layout_tree_build(tree_build_result.rebuilt_subtree_roots.size(), tree_build_result.layout_tree_update_escaped_rebuild_roots);

            // NB: Called during layout update.
            if (document_element && document_element->unsafe_layout_node())
                propagate_scrollbar_width_to_viewport(*document_element, *m_layout_root);

            if (tree_build_result.needs_another_build_pass)
                continue;

            set_needs_full_layout_tree_update(false);

            if constexpr (UPDATE_LAYOUT_DEBUG) {
                dbgln("TREEBUILD {} µs", timer.elapsed_time().to_microseconds());
            }

            if (reconcile_stale_list_item_counters_after_tree_build(tree_build_result.rebuilt_subtree_roots))
                continue;
        }

        if (document_element && document_element->unsafe_layout_node()) {
            propagate_principal_writing_mode_to_viewport(*document_element, *m_layout_root);
            propagate_overflow_to_viewport(*document_element, *m_layout_root);
        } else {
            m_layout_root->modify_computed_values([](auto& values) {
                values.set_overflow_x(CSS::Overflow::Auto);
                values.set_overflow_y(CSS::Overflow::Auto);
            });
        }

        recompute_containing_blocks_in_inclusive_subtree(layout_node_arena(), *m_layout_root);

        // The walk above re-derived every fact partial relayout boundary qualification depends
        // on, so pending changes that escaped classification are accounted for from here on.
        m_partial_relayout_invalidation.clear_escape(PartialRelayoutEscapeClearReason::FullLayoutPass);

        layout_node_arena().sync_enrolled_content_for_layout();
        Layout::LayoutRustBridge bridge;
        bridge.run_root_layout(
            *m_layout_root,
            viewport_rect.width(),
            viewport_rect.height(),
            should_collect_devtools_layout_data);
        Layout::RustFFI::layout_arena_rebuild_scrollable_overflow_contained_boxes(layout_node_arena().handle(), Layout::Node::slot_id(m_layout_root.ptr()));

        style_invalidation_counters().relayouts_performed++;

        Layout::RustFFI::layout_arena_set_needs_full_scrollable_overflow_recalculation(layout_node_arena().handle());

        ++m_full_layout_count;

        after_layout_commit(LayoutTreeChanged::Yes, LayoutCommitScope::Full);

        Layout::RustFFI::layout_arena_reset_layout_update_flags_in_subtree(
            layout_node_arena().handle(), Layout::Node::slot_id(m_layout_root.ptr()));

        if constexpr (UPDATE_LAYOUT_DEBUG) {
            dbgln("LAYOUT {} {} µs", to_string(reason), timer.elapsed_time().to_microseconds());
        }

        if (!m_query_containers_needing_container_query_evaluation_after_layout.is_empty()) {
            auto query_containers = exchange(m_query_containers_needing_container_query_evaluation_after_layout, {});
            for (auto& query_container : query_containers) {
                if (!query_container->is_connected())
                    continue;

                CSS::Invalidation::invalidate_descendant_styles_depending_on_size_container_query(query_container);
            }
        }

        if (needs_style_update_after_layout())
            continue;

        // A zone rebuild requested during layout tree construction runs as another pass.
        if (m_top_layer_needs_layout_zone_rebuild || !m_elements_with_pending_top_layer_membership_change.is_empty())
            continue;

        // Layout-only invalidations still need to be flushed before we can exit.
        if (layout_is_up_to_date())
            break;
    }

    if (needs_style_update_after_layout() || !layout_is_up_to_date()) {
        ++m_style_invalidation_counters.style_stabilization_bound_failures;
        VERIFY_NOT_REACHED();
    }
}

// Collect elements with content-visibility: auto. This is used in the HTML event loop to avoid traversing the whole tree every time.
void Document::collect_boxes_with_auto_content_visibility()
{
    Vector<Layout::RustFFI::NodeSlotId> boxes_with_auto_content_visibility;
    unsafe_layout_node()->for_each_in_inclusive_subtree([&](Layout::Node& node) {
        switch (node.kind()) {
        case Layout::RustFFI::NodeKind::SVGMaskBox:
        case Layout::RustFFI::NodeKind::SVGClipBox:
        case Layout::RustFFI::NodeKind::SVGPatternBox:
            return TraversalDecision::SkipChildrenAndContinue;
        default:
            break;
        }
        auto const* node_with_style = as_if<Layout::NodeWithStyle>(node);
        if (Painting::has_committed_box(node) && node.dom_node() && node.dom_node()->is_element()
            && node_with_style && node_with_style->content_visibility() == CSS::ContentVisibility::Auto)
            boxes_with_auto_content_visibility.append(Painting::committed_row_slot(node));
        return TraversalDecision::Continue;
    });
    paint_state().set_boxes_with_auto_content_visibility(move(boxes_with_auto_content_visibility));
}

void Document::clear_devtools_layout_inspection_data()
{
    clear_grid_highlighted_node(nullptr);
    clear_flexbox_highlighted_node(nullptr);
}

bool Document::layout_is_up_to_date() const
{
    if (!navigable() || navigable()->active_document().ptr() != this)
        return true;
    if (!m_layout_root)
        return false;
    return !m_layout_root->needs_layout_update()
        && !needs_layout_tree_update()
        && !child_needs_layout_tree_update()
        && !needs_full_layout_tree_update()
        && (!m_layout_node_arena || !Layout::RustFFI::layout_arena_has_partial_relayout_boundary_roots(m_layout_node_arena->handle()));
}

void Document::update_style_computer_viewport_rect()
{
    // A viewport unit is resolved against this and named by no word of a style input record.
    if (style_computer().viewport_rect_for_style_environment() != viewport_rect())
        bump_style_environment_version();
    style_computer().set_viewport_rect({}, viewport_rect());
}

void Document::set_quirks_mode(QuirksMode mode)
{
    if (m_quirks_mode == mode)
        return;
    m_quirks_mode = mode;

    // Quirks mode changes how id and class selectors match, so cached query results must not survive it.
    bump_dom_tree_version();

    // It also changes which case a rule cache buckets id and class selectors under, and brings a user
    // agent stylesheet with it, so no scope's rule cache and no element's style survives it either.
    style_computer().style_engine().set_fold_id_and_class_name_case(in_quirks_mode());
    style_scope().invalidate_style_cache();
    for_each_shadow_root([](auto& shadow_root) {
        shadow_root.style_scope().invalidate_style_cache();
    });
    record_style_environment_change();
}

void Document::set_needs_mathml_and_svg_user_agent_style_sheets()
{
    if (m_needs_mathml_and_svg_user_agent_style_sheets)
        return;
    m_needs_mathml_and_svg_user_agent_style_sheets = true;

    // Two sheets join the user-agent origin, so no scope's rule cache survives. The elements they
    // decide for are all in one of the two namespaces, and the element that brought them is
    // arriving, so nothing already styled needs marking.
    style_scope().invalidate_style_cache();
    for_each_shadow_root([](auto& shadow_root) {
        shadow_root.style_scope().invalidate_style_cache();
    });
}

void Document::invalidate_style_for_viewport_change()
{
    bool registered_initial_value_depends_on_viewport_metrics = false;
    auto invalidate_registered_initial_values = [&](auto& registrations) {
        for (auto& [_, registration] : registrations) {
            if (!registration.computed_initial_value_depends_on_viewport_metrics)
                continue;
            registration.computed_initial_value = nullptr;
            registration.computed_initial_value_depends_on_viewport_metrics = false;
            registered_initial_value_depends_on_viewport_metrics = true;
        }
    };
    invalidate_registered_initial_values(m_registered_property_set);
    invalidate_registered_initial_values(m_cached_registered_properties_from_css_property_rules);

    if (registered_initial_value_depends_on_viewport_metrics) {
        // A registered initial value is shared by every element that does not specify the custom
        // property, so its consumers cannot be identified from their computed styles.
        record_style_environment_change();
        return;
    }

    auto& style_engine = style_computer().style_engine();
    for (auto style_node : style_engine.viewport_dependent_style_nodes()) {
        auto element = style_computer().element_for_style_node(style_node.value());
        if (!element || !element->is_connected() || &element->document() != this)
            continue;
        style_engine.record_element_style_input_change(style_node);
    }

    for_each_shadow_including_inclusive_descendant([](Node& node) {
        auto* element = as_if<Element>(node);
        if (!element)
            return TraversalDecision::Continue;

        // Descendants that inherit changed values are reached by the normal inherited-style reaction path.
        // Container query conditions that resolved a container unit against the viewport have no
        // computed-value dependency for the retained style engine to discover.
        if (element->style_uses_if_css_function() || element->style_depends_on_viewport_metrics())
            element->document().style_computer().style_engine().record_element_style_input_change(element->style_node_id());

        return TraversalDecision::Continue;
    });
}

void Document::sample_animation_effects_needing_style_update()
{
    if (!m_needs_animated_style_update)
        return;

    VERIFY(!m_is_updating_animated_style);
    m_is_updating_animated_style = true;
    ScopeGuard clear_is_updating_animated_style = [&] {
        finish_animated_style_update();
    };

    GC::RootVector<GC::Ref<Animations::Animation>> animations;
    if (m_force_throttled_animation_style_update) {
        for (auto& animation : m_associated_animations) {
            if (animation.is_idle() || !animation.effect() || !is<Animations::KeyframeEffect>(*animation.effect()))
                continue;
            animations.append(animation);
        }
    } else {
        for (auto& effect : m_effects_needing_animated_style_update) {
            auto animation = effect.associated_animation();
            if (!animation || animation->is_idle())
                continue;
            animations.append(*animation);
        }
    }
    m_effects_needing_animated_style_update.clear();
    m_needs_animated_style_update = false;

    if (animations.is_empty()) {
        m_force_throttled_animation_style_update = false;
        // A compositor-driven effect can remain throttled when every dirty effect was cancelled before this sample.
        // Preserve the flag so a later style or geometry read can still request a current main-thread sample.
        return;
    }

    bool has_requested_observation_sample = any_of(animations, [](auto const& animation) {
        return static_cast<Animations::KeyframeEffect&>(*animation->effect()).observation_sample_requested();
    });

    GC::RootVector<GC::Ref<Animations::AnimationTimeline>> timelines_with_current_time_override;
    if (m_force_throttled_animation_style_update || has_requested_observation_sample) {
        for (auto& timeline : m_associated_animation_timelines)
            timelines_with_current_time_override.append(timeline);
        for (auto& timeline : timelines_with_current_time_override)
            timeline->set_current_time_override_for_style_sampling(timeline->current_time_for_observation());
    }
    ScopeGuard clear_current_time_overrides = [&] {
        for (auto& timeline : timelines_with_current_time_override)
            timeline->clear_current_time_override_for_style_sampling();
    };

    Animations::AnimationUpdateContext context;

    quick_sort(animations, [](GC::Ref<Animations::Animation>& a, GC::Ref<Animations::Animation>& b) {
        auto& a_effect = as<Animations::KeyframeEffect>(*a->effect());
        auto& b_effect = as<Animations::KeyframeEffect>(*b->effect());
        return Animations::KeyframeEffect::composite_order(a_effect, b_effect) < 0;
    });

    bool has_throttled_animation_style_update = m_force_throttled_animation_style_update ? false : m_has_throttled_animation_style_update;
    for (auto& animation : animations) {
        auto& effect = static_cast<Animations::KeyframeEffect&>(*animation->effect());
        bool observation_sample_requested = effect.consume_observation_sample_request();
        if (effect.can_skip_per_frame_style_update()) {
            has_throttled_animation_style_update = true;
            if (!m_force_throttled_animation_style_update && !observation_sample_requested)
                continue;
        }
        animation->effect()->update_computed_properties(context);
    }

    m_has_throttled_animation_style_update = has_throttled_animation_style_update;
    if (m_force_throttled_animation_style_update)
        m_last_forced_throttled_animation_style_update_task_generation = relevant_settings_object().responsible_event_loop().task_generation();
    m_force_throttled_animation_style_update = false;
}

void Document::flush_throttled_animation_style_update()
{
    if (!m_has_throttled_animation_style_update)
        return;
    auto task_generation = relevant_settings_object().responsible_event_loop().task_generation();
    if (!m_needs_animated_style_update
        && m_last_forced_throttled_animation_style_update_task_generation == task_generation)
        return;
    m_has_throttled_animation_style_update = false;
    m_force_throttled_animation_style_update = true;
    m_needs_animated_style_update = true;
}

void Document::flush_throttled_animation_style_update_for_node(Node const& node)
{
    auto task_generation = relevant_settings_object().responsible_event_loop().task_generation();
    for (auto& animation : m_associated_animations) {
        if (!animation.effect() || !is<Animations::KeyframeEffect>(*animation.effect()))
            continue;
        auto& effect = static_cast<Animations::KeyframeEffect&>(*animation.effect());
        auto target = effect.target();
        if (!target
            || (!target->is_shadow_including_inclusive_ancestor_of(node) && !node.is_shadow_including_inclusive_ancestor_of(*target))
            || !effect.can_skip_per_frame_style_update())
            continue;
        if (m_is_updating_animated_style) {
            m_effects_needing_animated_style_update_after_current_update.set(effect);
        } else {
            effect.request_element_scoped_observation_sample(task_generation);
        }
    }
}

void Document::request_reentrant_animation_style_flush_for_testing(Badge<Internals::Internals>, Node const& node)
{
    VERIFY(!m_is_updating_animated_style);
    m_is_updating_animated_style = true;
    ScopeGuard clear_is_updating_animated_style = [&] {
        finish_animated_style_update();
    };
    flush_throttled_animation_style_update_for_node(node);
}

bool Document::run_empty_animation_style_update_for_testing(Badge<Internals::Internals>)
{
    VERIFY(!m_is_updating_animated_style);
    m_needs_animated_style_update = true;
    m_effects_needing_animated_style_update.clear();
    sample_animation_effects_needing_style_update();
    return m_has_throttled_animation_style_update;
}

void Document::stop_compositor_animation_timers()
{
    if (m_compositor_animation_wakeup_timer) {
        m_compositor_animation_wakeup_timer->stop();
        m_compositor_animation_wakeup_deadline.clear();
    }
    if (m_compositor_animation_observation_timer)
        m_compositor_animation_observation_timer->stop();
}

void Document::arm_compositor_animation_timers_for_testing(Badge<Internals::Internals>)
{
    stop_compositor_animation_timers();
    schedule_compositor_animation_wakeup(NumericLimits<int>::max());
    m_compositor_animation_observation_timer = Core::Timer::create_single_shot(NumericLimits<int>::max(), GC::weak_callback(*this, [](auto&) { }));
    m_compositor_animation_observation_timer->start();
}

bool Document::compositor_animation_wakeup_timer_is_active() const
{
    return m_compositor_animation_wakeup_timer && m_compositor_animation_wakeup_timer->is_active();
}

bool Document::compositor_animation_observation_timer_is_active() const
{
    return m_compositor_animation_observation_timer && m_compositor_animation_observation_timer->is_active();
}

void Document::throttled_animation_visibility_changed()
{
    if (!m_has_throttled_animation_style_update)
        return;
    flush_throttled_animation_style_update();
    page().client().request_frame();
}

void Document::set_needs_animated_style_update(Animations::KeyframeEffect& effect)
{
    m_effects_needing_animated_style_update.set(effect);
    if (m_needs_animated_style_update)
        return;

    m_needs_animated_style_update = true;

    auto navigable = this->navigable();
    if (navigable && navigable->has_inclusive_ancestor_with_visibility_hidden())
        return;

    page().client().request_frame();
}

void Document::finish_animated_style_update()
{
    VERIFY(m_is_updating_animated_style);
    m_is_updating_animated_style = false;

    GC::RootVector<GC::Ref<Animations::KeyframeEffect>> effects;
    for (auto& effect : m_effects_needing_animated_style_update_after_current_update)
        effects.append(effect);
    m_effects_needing_animated_style_update_after_current_update.clear();

    for (auto& effect : effects)
        effect->request_observation_sample();
}

void Document::update_scrollable_overflow(ScrollableOverflowDerivedStructureUpdates derived_structure_updates)
{
    if (!m_layout_node_arena)
        return;

    // The update reads each box's scroll-offset flag in place of the offset it mirrors.
    static bool const verify_scroll_offset_flags = getenv("LIBWEB_VERIFY_SCROLL_OFFSET_FLAGS") != nullptr;
    if (verify_scroll_offset_flags && m_layout_root) {
        m_layout_root->for_each_in_inclusive_subtree([](Layout::Node const& node) {
            node.verify_has_scroll_offset_flag();
            return TraversalDecision::Continue;
        });
    }

    auto outcome = Painting::rust_update_scrollable_overflow(*this,
        derived_structure_updates == ScrollableOverflowDerivedStructureUpdates::HandledByFullLayoutCommit);
    if (!outcome.performed_recalculation)
        return;

    style_invalidation_counters().scrollable_overflow_recalculations++;

    if (derived_structure_updates != ScrollableOverflowDerivedStructureUpdates::UpdateAfterMeasure)
        return;

    // Nothing derived from scrollable overflow needs updating. In particular, this keeps transform
    // changes that ride the accumulated-visual-context value-update path free of display list
    // re-recording when the overflow they produce is unchanged.
    if (!outcome.any_overflow_changed)
        return;

    if (outcome.any_has_scrollable_overflow_flipped) {
        set_needs_accumulated_visual_contexts_update(true);
    } else if (!m_needs_accumulated_visual_contexts_update) {
        // Sticky insets only depend on scrollport geometry and which ancestor is scrollable, neither of
        // which changes without a flip; the constraints capture the scroll ancestor's scrollable
        // overflow size though, so they have to be refreshed. When a full visual context rebuild is
        // already pending it recaptures constraints anyway, and skipping the refresh then also avoids
        // touching scroll nodes whose committed rows a subtree relayout may have replaced.
        paint_state().refresh_sticky_constraints(*this);
    }
    set_needs_to_record_display_list();
    m_document->set_needs_repaint();
}

void Document::update_paint_and_hit_testing_properties_if_needed()
{
    // NB: Called during paint property resolution.
    if (m_needs_accumulated_visual_contexts_update) {
        m_needs_accumulated_visual_contexts_update = false;
        if (has_committed_viewport_box())
            paint_state().update_accumulated_visual_contexts(*this);
    }

    // Scroll nodes are (re)created by the visual context tree build above, so scroll offsets and
    // the snapshot must be derived only after structure work is done.
    if (has_committed_viewport_box())
        paint_state().refresh_scroll_state(*this);
}

bool Document::can_compute_client_rects_without_accumulated_visual_contexts_update(Layout::Node const& layout_node) const
{
    if (!m_needs_accumulated_visual_contexts_update || !m_layout_root)
        return false;

    for (auto const* node = &layout_node; node; node = node->parent()) {
        if (node->is_svg_box() || node->is_svg_svg_box() || node->is_svg_foreign_object_box())
            return false;

        auto const* node_with_style = as_if<Layout::NodeWithStyle>(*node);
        if (!node_with_style)
            continue;
        if (node_with_style->has_css_transform() || node_with_style->perspective().has_value() || node_with_style->is_sticky_position())
            return false;
        if (auto const* box = as_if<Layout::Box>(*node);
            box && (box->compensates_for_horizontal_scroll() || box->compensates_for_vertical_scroll()))
            return false;
        // A scroll container's contents move, but its own border box does not.
        if (node != &layout_node) {
            if (!Painting::scroll_offset(*node).is_zero())
                return false;
        }
    }
    return true;
}

void Document::set_normal_link_color(Optional<Color> color)
{
    if (m_normal_link_color == color)
        return;
    m_normal_link_color = color;
    CSS::Invalidation::invalidate_style_after_legacy_link_color_change(*this);
}

void Document::set_active_link_color(Optional<Color> color)
{
    if (m_active_link_color == color)
        return;
    m_active_link_color = color;
    CSS::Invalidation::invalidate_style_after_legacy_link_color_change(*this);
}

void Document::set_visited_link_color(Optional<Color> color)
{
    if (m_visited_link_color == color)
        return;
    m_visited_link_color = color;
    CSS::Invalidation::invalidate_style_after_legacy_link_color_change(*this);
}

Optional<Vector<Utf16FlyString> const&> Document::supported_color_schemes() const
{
    return m_supported_color_schemes;
}

void Document::set_supported_color_schemes(Vector<Utf16FlyString> supported_color_schemes)
{
    set_supported_color_schemes(Optional<Vector<Utf16FlyString>> { move(supported_color_schemes) });
}

void Document::set_supported_color_schemes(Optional<Vector<Utf16FlyString>> supported_color_schemes)
{
    if (m_supported_color_schemes == supported_color_schemes)
        return;

    m_supported_color_schemes = move(supported_color_schemes);
    record_style_environment_change();
    set_needs_media_query_evaluation();
}

// https://html.spec.whatwg.org/multipage/semantics.html#meta-color-scheme
void Document::obtain_supported_color_schemes()
{
    Optional<Vector<Utf16FlyString>> supported_color_schemes;

    // 1. Let candidate elements be the list of all meta elements that meet the following criteria, in tree order:
    for_each_in_subtree_of_type<HTML::HTMLMetaElement>([&](HTML::HTMLMetaElement& element) {
        //     * the element is in a document tree;
        //     * the element has a name attribute, whose value is an ASCII case-insensitive match for color-scheme; and
        //     * the element has a content attribute.

        // 2. For each element in candidate elements:
        auto content = element.attribute(HTML::AttributeNames::content);
        if (element.name().has_value() && element.name()->equals_ignoring_ascii_case(u"color-scheme"sv) && content.has_value()) {
            // 1. Let parsed be the result of parsing a list of component values given the value of element's content attribute.
            auto context = CSS::Parser::ParsingParams { document() };
            auto parsed = parse_css_value(context, content.value(), CSS::PropertyID::ColorScheme);

            // 2. If parsed is a valid CSS 'color-scheme' property value, then return parsed.
            if (!parsed.is_null() && parsed->is_color_scheme()) {
                supported_color_schemes = parsed->as_color_scheme().schemes();
                return TraversalDecision::Break;
            }
        }

        return TraversalDecision::Continue;
    });

    // 3. Return null.
    set_supported_color_schemes(move(supported_color_schemes));
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
        if (element.name().has_value() && element.name()->equals_ignoring_ascii_case(u"theme-color"sv) && content.has_value()) {
            // 1. If element has a media attribute and the value of element's media attribute does not match the environment, then continue.
            auto context = CSS::Parser::ParsingParams { document() };
            auto media = element.attribute(HTML::AttributeNames::media);
            if (media.has_value()) {
                auto query = parse_media_query(context, media.value());
                if (query.is_null() || !query->evaluate(*this))
                    return TraversalDecision::Continue;
            }

            // 2. Let value be the result of stripping leading and trailing ASCII whitespace from the value of element's content attribute.
            auto value = content->utf16_view().trim(Infra::ASCII_WHITESPACE);

            // 3. Let color be the result of parsing value.
            auto css_value = parse_css_value(context, value, CSS::PropertyID::Color);

            // 4. If color is not failure, then return color.
            if (!css_value.is_null() && css_value->has_color()) {
                CSS::ColorResolutionContext color_resolution_context {};
                // NB: Called during theme color computation, layout may be stale.
                if (html_element() && html_element()->unsafe_layout_node()) {
                    color_resolution_context = CSS::ColorResolutionContext::for_layout_node_with_style(*html_element()->unsafe_layout_node());
                }

                theme_color = css_value->to_color(color_resolution_context).value();
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

Layout::Viewport const* Document::unsafe_layout_node() const
{
    return static_cast<Layout::Viewport const*>(Node::unsafe_layout_node());
}

Layout::Viewport* Document::unsafe_layout_node()
{
    return static_cast<Layout::Viewport*>(Node::unsafe_layout_node());
}

bool Document::has_committed_viewport_box() const
{
    return m_layout_root && Painting::has_committed_box(*m_layout_root);
}

void Document::set_inspected_node(GC::Ptr<Node> node)
{
    m_inspected_node = node;
}

void Document::set_highlighted_node(GC::Ptr<Node> node, Optional<CSS::PseudoElement> pseudo_element)
{
    if (m_highlighted_node == node && m_highlighted_pseudo_element == pseudo_element)
        return;

    if (auto layout_node = highlighted_layout_node(); layout_node && Painting::has_committed_box(*layout_node))
        Painting::set_needs_repaint(*layout_node);

    m_highlighted_node = node;
    m_highlighted_pseudo_element = pseudo_element;

    if (auto layout_node = highlighted_layout_node(); layout_node && Painting::has_committed_box(*layout_node))
        Painting::set_needs_repaint(*layout_node);
}

void Document::set_grid_highlighted_node(GC::Ptr<Node> node, Painting::GridInspectorOverlayOptions options)
{
    if (!node)
        return;

    for (auto& grid_highlight : m_grid_highlights) {
        if (grid_highlight.node != node)
            continue;

        grid_highlight.options = options;
        node->set_needs_repaint();
        return;
    }

    m_grid_highlights.append({ node, options });
    node->set_needs_repaint();
}

void Document::set_flexbox_highlighted_node(GC::Ptr<Node> node, Painting::FlexboxInspectorOverlayOptions options)
{
    if (!node)
        return;

    for (auto& flexbox_highlight : m_flexbox_highlights) {
        if (flexbox_highlight.node != node)
            continue;

        flexbox_highlight.options = options;
        node->set_needs_repaint();
        return;
    }

    m_flexbox_highlights.append({ node, options });
    node->set_needs_repaint();
}

void Document::clear_flexbox_highlighted_node(GC::Ptr<Node> node)
{
    if (!node) {
        for (auto const& flexbox_highlight : m_flexbox_highlights) {
            if (flexbox_highlight.node)
                flexbox_highlight.node->set_needs_repaint();
        }
        m_flexbox_highlights.clear();
        return;
    }

    auto old_size = m_flexbox_highlights.size();
    m_flexbox_highlights.remove_all_matching([&](auto const& flexbox_highlight) {
        return flexbox_highlight.node == node;
    });

    if (m_flexbox_highlights.size() != old_size)
        node->set_needs_repaint();
}

void Document::clear_grid_highlighted_node(GC::Ptr<Node> node)
{
    if (!node) {
        for (auto const& grid_highlight : m_grid_highlights) {
            if (grid_highlight.node)
                grid_highlight.node->set_needs_repaint();
        }
        m_grid_highlights.clear();
        return;
    }

    auto old_size = m_grid_highlights.size();
    m_grid_highlights.remove_all_matching([&](auto const& grid_highlight) {
        return grid_highlight.node == node;
    });

    if (m_grid_highlights.size() != old_size)
        node->set_needs_repaint();
}

Layout::Node* Document::highlighted_layout_node()
{
    if (!m_highlighted_node)
        return nullptr;

    // NB: Called during painting inside update_layout().
    if (!m_highlighted_pseudo_element.has_value() || !m_highlighted_node->is_element())
        return m_highlighted_node->unsafe_layout_node();

    auto const& element = static_cast<Element const&>(*m_highlighted_node);
    return element.pseudo_element_unsafe_layout_node(m_highlighted_pseudo_element.value());
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

static void populate_mouse_event_options_from_hover_event_data(UIEvents::MouseEventOptions& options, HoverEventData const& hover_event_data, GC::Ptr<HTML::WindowProxy> window_proxy)
{
    options.ctrl_key = hover_event_data.modifiers & UIEvents::KeyModifier::Mod_Ctrl;
    options.shift_key = hover_event_data.modifiers & UIEvents::KeyModifier::Mod_Shift;
    options.alt_key = hover_event_data.modifiers & UIEvents::KeyModifier::Mod_Alt;
    options.meta_key = hover_event_data.modifiers & UIEvents::KeyModifier::Mod_Super;
    options.screen_x = hover_event_data.screen_position.x().to_double();
    options.screen_y = hover_event_data.screen_position.y().to_double();
    options.client_x = hover_event_data.viewport_position.x().to_double();
    options.client_y = hover_event_data.viewport_position.y().to_double();
    options.view = window_proxy;
    if (hover_event_data.movement.has_value()) {
        options.movement_x = hover_event_data.movement->x().to_double();
        options.movement_y = hover_event_data.movement->y().to_double();
    }
    options.button = UIEvents::mouse_button_to_button_code(static_cast<UIEvents::MouseButton>(hover_event_data.button));
    options.buttons = hover_event_data.buttons;
}

static CSSPixelPoint hover_event_page_offset(Optional<HoverEventData> const& hover_event_data)
{
    if (hover_event_data.has_value())
        return hover_event_data->page_offset;
    return {};
}

// https://drafts.csswg.org/cssom-view/#dom-mouseevent-offsetx
static CSSPixelPoint compute_mouse_event_offset(CSSPixelPoint position, Layout::Node const& layout_node)
{
    auto inverse_transform_point = [](Layout::Node const& layout_node, CSSPixelPoint position) -> Optional<CSSPixelPoint> {
        auto& document = layout_node.document();
        if (!document.has_committed_viewport_box())
            return {};
        auto pixel_ratio = static_cast<float>(document.page().client().device_pixels_per_css_pixel());
        auto visual_context_tree = document.visual_context_tree();
        auto transformed_position = visual_context_tree.inverse_transform_point(
            Painting::accumulated_visual_context(layout_node).spatial, position.to_type<float>() * pixel_ratio);
        return (transformed_position / pixel_ratio).to_type<CSSPixels>();
    };

    CSSPixelPoint offset_position = position;
    if (auto transformed_position = inverse_transform_point(layout_node, position); transformed_position.has_value())
        offset_position = *transformed_position;

    auto const top_left_of_layout_node = Painting::box_type_agnostic_position(layout_node);
    return offset_position - top_left_of_layout_node;
}

static CSSPixelPoint hover_event_offset_for_target(Optional<HoverEventData> const& hover_event_data, Node const& target)
{
    if (!hover_event_data.has_value())
        return {};

    // Boundary events are dispatched in a batch, and earlier listeners in the batch can invalidate layout. The event
    // offsets still need to be based on the layout tree that was used for the platform hit-test, so avoid helpers that
    // assert layout is up to date.
    auto* layout_node = target.unsafe_layout_node();
    if (!layout_node)
        return hover_event_data->viewport_position;

    if (!Painting::has_committed_box(*layout_node))
        return hover_event_data->viewport_position;

    return compute_mouse_event_offset(hover_event_data->page_offset, *layout_node);
}

static void mark_mouse_transition_event_as_trusted_if_needed(Event& event, Optional<HoverEventData> const& hover_event_data)
{
    if (hover_event_data.has_value())
        event.set_is_trusted(true);
}

void Document::set_hovered_node(GC::Ptr<Node> node, Optional<HoverEventData> hover_event_data)
{
    if (m_hovered_node == node)
        return;

    GC::Ptr<Node> old_hovered_node = move(m_hovered_node);
    auto* common_ancestor = find_common_ancestor(old_hovered_node.ptr(), node.ptr());
    GC::Ptr<HTML::WindowProxy> window_proxy;
    if (auto navigable = this->navigable())
        window_proxy = navigable->active_window_proxy();
    auto page_offset = hover_event_page_offset(hover_event_data);

    struct HoverEventTarget {
        GC::Ref<Node> node;
        CSSPixelPoint offset;
    };

    auto make_hover_event_target = [&](Node& target) {
        return HoverEventTarget { target, hover_event_offset_for_target(hover_event_data, target) };
    };

    // Boundary event listeners can invalidate layout while the batch is still being dispatched. Compute all offsets
    // against the layout and paint trees from the original platform hit-test before firing the first event.
    Optional<CSSPixelPoint> old_hovered_node_offset;
    if (old_hovered_node)
        old_hovered_node_offset = hover_event_offset_for_target(hover_event_data, *old_hovered_node);

    Optional<CSSPixelPoint> hovered_node_offset;
    if (node)
        hovered_node_offset = hover_event_offset_for_target(hover_event_data, *node);

    Vector<HoverEventTarget> pointer_leave_targets;
    if (old_hovered_node && (!node || !node->is_descendant_of(*old_hovered_node))) {
        for (auto target = old_hovered_node; target && target.ptr() != common_ancestor; target = target->parent())
            pointer_leave_targets.append(make_hover_event_target(*target));
    }

    Vector<HoverEventTarget> mouse_leave_targets;
    if (old_hovered_node && (!node || !node->is_descendant_of(*old_hovered_node))) {
        for (auto target = old_hovered_node; target && target.ptr() != common_ancestor; target = target->parent_or_shadow_host())
            mouse_leave_targets.append(make_hover_event_target(*target));
    }

    Vector<HoverEventTarget> entered_ancestors;
    if (node && (!old_hovered_node || !node->is_ancestor_of(*old_hovered_node))) {
        for (auto target = node; target && target.ptr() != common_ancestor; target = target->parent_or_shadow_host())
            entered_ancestors.append(make_hover_event_target(*target));
    }

    CSS::Invalidation::invalidate_style_after_pseudo_class_state_change(CSS::PseudoClass::Hover, old_hovered_node, node);

    m_hovered_node = node;

    auto current_event_time_stamp = [&] {
        return HighResolutionTime::current_high_resolution_time(relevant_global_object(*this));
    };
    auto create_pointer_event = [&](Utf16FlyString const& event_name, UIEvents::PointerEventOptions options, CSSPixelPoint offset) {
        auto utf8_name = event_name.to_utf16_string().to_utf8();
        return UIEvents::PointerEvent::create(
            MUST(FlyString::from_utf8(utf8_name.bytes_as_string_view())), move(options),
            page_offset.x().to_double(), page_offset.y().to_double(),
            offset.x().to_double(), offset.y().to_double(),
            current_event_time_stamp());
    };
    auto create_mouse_event = [&](Utf16FlyString const& event_name, UIEvents::MouseEventOptions const& options, CSSPixelPoint offset) {
        auto utf8_name = event_name.to_utf16_string().to_utf8();
        return UIEvents::MouseEvent::create(
            MUST(FlyString::from_utf8(utf8_name.bytes_as_string_view())), options,
            page_offset.x().to_double(), page_offset.y().to_double(),
            offset.x().to_double(), offset.y().to_double(),
            current_event_time_stamp());
    };

    // https://w3c.github.io/pointerevents/#the-pointerout-event
    if (old_hovered_node && old_hovered_node != m_hovered_node) {
        UIEvents::PointerEventOptions pointer_event_options;
        pointer_event_options.bubbles = true;
        pointer_event_options.cancelable = true;
        pointer_event_options.composed = true;
        pointer_event_options.related_target = m_hovered_node;
        pointer_event_options.is_primary = true;
        pointer_event_options.pointer_type = UIEvents::PointerTypes::Mouse;
        pointer_event_options.view = window_proxy;
        if (hover_event_data.has_value())
            populate_mouse_event_options_from_hover_event_data(pointer_event_options, *hover_event_data, window_proxy);
        auto offset = *old_hovered_node_offset;
        auto event = create_pointer_event(UIEvents::EventNames::pointerout, move(pointer_event_options), offset);
        mark_mouse_transition_event_as_trusted_if_needed(event, hover_event_data);
        old_hovered_node->dispatch_event(event);
    }

    // https://w3c.github.io/uievents/#mouseout
    if (old_hovered_node && old_hovered_node != m_hovered_node) {
        UIEvents::MouseEventOptions mouse_event_options;
        mouse_event_options.bubbles = true;
        mouse_event_options.cancelable = true;
        mouse_event_options.composed = true;
        mouse_event_options.related_target = m_hovered_node;
        mouse_event_options.view = window_proxy;
        if (hover_event_data.has_value())
            populate_mouse_event_options_from_hover_event_data(mouse_event_options, *hover_event_data, window_proxy);
        auto offset = *old_hovered_node_offset;
        auto event = create_mouse_event(UIEvents::EventNames::mouseout, mouse_event_options, offset);
        mark_mouse_transition_event_as_trusted_if_needed(event, hover_event_data);
        old_hovered_node->dispatch_event(event);
    }

    // https://w3c.github.io/pointerevents/#the-pointerleave-event
    if (!pointer_leave_targets.is_empty()) {
        for (auto const& target : pointer_leave_targets) {
            UIEvents::PointerEventOptions pointer_event_options;
            pointer_event_options.related_target = m_hovered_node;
            pointer_event_options.is_primary = true;
            pointer_event_options.pointer_type = UIEvents::PointerTypes::Mouse;
            pointer_event_options.view = window_proxy;
            if (hover_event_data.has_value())
                populate_mouse_event_options_from_hover_event_data(pointer_event_options, *hover_event_data, window_proxy);
            auto offset = target.offset;
            auto event = create_pointer_event(UIEvents::EventNames::pointerleave, move(pointer_event_options), offset);
            mark_mouse_transition_event_as_trusted_if_needed(event, hover_event_data);
            target.node->dispatch_event(event);
        }
    }

    // https://w3c.github.io/uievents/#mouseleave
    if (!mouse_leave_targets.is_empty()) {
        for (auto const& target : mouse_leave_targets) {
            UIEvents::MouseEventOptions mouse_event_options;
            mouse_event_options.related_target = m_hovered_node;
            if (hover_event_data.has_value())
                populate_mouse_event_options_from_hover_event_data(mouse_event_options, *hover_event_data, window_proxy);
            auto offset = target.offset;
            auto event = create_mouse_event(UIEvents::EventNames::mouseleave, mouse_event_options, offset);
            mark_mouse_transition_event_as_trusted_if_needed(event, hover_event_data);
            target.node->dispatch_event(event);
        }
    }

    // https://w3c.github.io/pointerevents/#the-pointerover-event
    if (m_hovered_node && m_hovered_node != old_hovered_node) {
        UIEvents::PointerEventOptions pointer_event_options;
        pointer_event_options.bubbles = true;
        pointer_event_options.cancelable = true;
        pointer_event_options.composed = true;
        pointer_event_options.related_target = old_hovered_node;
        pointer_event_options.is_primary = true;
        pointer_event_options.pointer_type = UIEvents::PointerTypes::Mouse;
        pointer_event_options.view = window_proxy;
        if (hover_event_data.has_value())
            populate_mouse_event_options_from_hover_event_data(pointer_event_options, *hover_event_data, window_proxy);
        auto offset = *hovered_node_offset;
        auto event = create_pointer_event(UIEvents::EventNames::pointerover, move(pointer_event_options), offset);
        mark_mouse_transition_event_as_trusted_if_needed(event, hover_event_data);
        m_hovered_node->dispatch_event(event);
    }

    // https://w3c.github.io/uievents/#mouseover
    if (m_hovered_node && m_hovered_node != old_hovered_node) {
        UIEvents::MouseEventOptions mouse_event_options;
        mouse_event_options.bubbles = true;
        mouse_event_options.cancelable = true;
        mouse_event_options.composed = true;
        mouse_event_options.related_target = old_hovered_node;
        mouse_event_options.view = window_proxy;
        if (hover_event_data.has_value())
            populate_mouse_event_options_from_hover_event_data(mouse_event_options, *hover_event_data, window_proxy);
        auto offset = *hovered_node_offset;
        auto event = create_mouse_event(UIEvents::EventNames::mouseover, mouse_event_options, offset);
        mark_mouse_transition_event_as_trusted_if_needed(event, hover_event_data);
        m_hovered_node->dispatch_event(event);
    }

    // https://w3c.github.io/pointerevents/#the-pointerenter-event
    // https://w3c.github.io/uievents/#mouseenter
    // Enter events are dispatched from ancestor to descendant.
    // Leave events are dispatched in the opposite order.
    if (!entered_ancestors.is_empty()) {
        for (auto target : entered_ancestors.in_reverse()) {
            UIEvents::PointerEventOptions pointer_event_options;
            pointer_event_options.related_target = old_hovered_node;
            pointer_event_options.is_primary = true;
            pointer_event_options.pointer_type = UIEvents::PointerTypes::Mouse;
            pointer_event_options.view = window_proxy;
            if (hover_event_data.has_value())
                populate_mouse_event_options_from_hover_event_data(pointer_event_options, *hover_event_data, window_proxy);
            auto offset = target.offset;
            auto pointer_event = create_pointer_event(UIEvents::EventNames::pointerenter, move(pointer_event_options), offset);
            mark_mouse_transition_event_as_trusted_if_needed(pointer_event, hover_event_data);
            target.node->dispatch_event(pointer_event);
            UIEvents::MouseEventOptions mouse_event_options;
            mouse_event_options.related_target = old_hovered_node;
            if (hover_event_data.has_value())
                populate_mouse_event_options_from_hover_event_data(mouse_event_options, *hover_event_data, window_proxy);
            auto mouse_event = create_mouse_event(UIEvents::EventNames::mouseenter, mouse_event_options, offset);
            mark_mouse_transition_event_as_trusted_if_needed(mouse_event, hover_event_data);
            target.node->dispatch_event(mouse_event);
        }
    }
}

// https://html.spec.whatwg.org/multipage/dom.html#dom-document-getelementsbyname
GC::Ref<NodeList> Document::get_elements_by_name(Utf16View name)
{
    auto name_copy = Utf16String::from_utf16(name);
    return LiveNodeList::create(*this, LiveNodeList::Scope::Descendants, [name = move(name_copy)](auto const& node) {
        if (!is<HTML::HTMLElement>(node))
            return false;
        auto element_name = as<DOM::Element>(node).get_attribute(HTML::AttributeNames::name);
        return element_name.has_value() && !element_name->is_empty() && element_name->utf16_view() == name.utf16_view();
    });
}

// https://html.spec.whatwg.org/multipage/obsolete.html#dom-document-applets
GC::Ref<HTMLCollection> Document::applets()
{
    if (!m_applets)
        m_applets = HTMLCollection::create(*this, HTMLCollection::Scope::Descendants, [](auto&) { return false; }, HTMLCollection::AttributeInvalidationType::None);
    return *m_applets;
}

// https://html.spec.whatwg.org/multipage/obsolete.html#dom-document-anchors
GC::Ref<HTMLCollection> Document::anchors()
{
    if (!m_anchors) {
        m_anchors = HTMLCollection::create(*this, HTMLCollection::Scope::Descendants, [](Element const& element) { return is<HTML::HTMLAnchorElement>(element) && element.name().has_value(); }, HTMLCollection::AttributeInvalidationType::Name);
    }
    return *m_anchors;
}

// https://html.spec.whatwg.org/multipage/dom.html#dom-document-images
GC::Ref<HTMLCollection> Document::images()
{
    if (!m_images) {
        m_images = HTMLCollection::create(*this, HTMLCollection::Scope::Descendants, [](Element const& element) { return is<HTML::HTMLImageElement>(element); }, HTMLCollection::AttributeInvalidationType::None);
    }
    return *m_images;
}

// https://html.spec.whatwg.org/multipage/dom.html#dom-document-embeds
GC::Ref<HTMLCollection> Document::embeds()
{
    if (!m_embeds) {
        m_embeds = HTMLCollection::create(*this, HTMLCollection::Scope::Descendants, [](Element const& element) { return is<HTML::HTMLEmbedElement>(element); }, HTMLCollection::AttributeInvalidationType::None);
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
        m_links = HTMLCollection::create(*this, HTMLCollection::Scope::Descendants, [](Element const& element) { return (is<HTML::HTMLAnchorElement>(element) || is<HTML::HTMLAreaElement>(element)) && element.has_attribute(HTML::AttributeNames::href); }, HTMLCollection::AttributeInvalidationType::Href);
    }
    return *m_links;
}

// https://html.spec.whatwg.org/multipage/dom.html#dom-document-forms
GC::Ref<HTMLCollection> Document::forms()
{
    if (!m_forms) {
        m_forms = HTMLCollection::create(*this, HTMLCollection::Scope::Descendants, [](Element const& element) { return is<HTML::HTMLFormElement>(element); }, HTMLCollection::AttributeInvalidationType::None);
    }
    return *m_forms;
}

// https://html.spec.whatwg.org/multipage/dom.html#dom-document-scripts
GC::Ref<HTMLCollection> Document::scripts()
{
    if (!m_scripts) {
        m_scripts = HTMLCollection::create(*this, HTMLCollection::Scope::Descendants, [](Element const& element) { return is<HTML::HTMLScriptElement>(element); }, HTMLCollection::AttributeInvalidationType::None);
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
    return m_fonts;
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
    auto& global_scope = HTML::relevant_window_or_worker_global_scope(*m_relevant_global_event_target);
    return HTML::relevant_settings_object(global_scope);
}

// https://dom.spec.whatwg.org/#dom-document-createelement
WebIDL::ExceptionOr<GC::Ref<Element>> Document::create_element(Utf16FlyString const& local_name, ElementCreationOptions const& options)
{
    auto normalized_local_name = local_name;
    // 1. If localName is not a valid element local name, then throw an "InvalidCharacterError" DOMException.
    if (!is_valid_element_local_name(local_name.view()))
        return WebIDL::InvalidCharacterError::create("Invalid character in tag name."_utf16);

    // 2. If this is an HTML document, then set localName to localName in ASCII lowercase.
    if (document_type() == Type::HTML)
        normalized_local_name = normalized_local_name.to_ascii_lowercase();

    // 3. Let registry and is be the result of flattening element creation options given options and this.
    auto [registry, is_value] = TRY(flatten_element_creation_options(options));

    // 4. Let namespace be the HTML namespace, if this is an HTML document or this’s content type is
    //    "application/xhtml+xml"; otherwise null.
    Optional<Utf16FlyString> namespace_;
    if (document_type() == Type::HTML || content_type() == u"application/xhtml+xml"sv)
        namespace_ = Namespace::HTML;

    // 5. Return the result of creating an element given this, localName, namespace, null, is, true, and registry.
    return TRY(DOM::create_element(*this, move(normalized_local_name), move(namespace_), {}, move(is_value), true, registry));
}

WebIDL::ExceptionOr<GC::Ref<Element>> Document::create_element(Utf16FlyString const& local_name, Variant<Utf16String, ElementCreationOptions> const& options)
{
    ElementCreationOptions element_creation_options;
    options.visit(
        [&](Utf16String const& is) {
            element_creation_options.is = is;
        },
        [&](ElementCreationOptions const& options) {
            element_creation_options = options;
        });
    return create_element(local_name, element_creation_options);
}

// https://dom.spec.whatwg.org/#dom-document-createelementns
// https://dom.spec.whatwg.org/#internal-createelementns-steps
WebIDL::ExceptionOr<GC::Ref<Element>> Document::create_element_ns(Optional<Utf16String> const& namespace_, Utf16String const& qualified_name, ElementCreationOptions const& options)
{
    // 1. Let (namespace, prefix, localName) be the result of validating and extracting namespace and qualifiedName
    //    given "element".
    auto utf8_qualified_name = qualified_name.to_utf8();
    auto fly_qualified_name = MUST(FlyString::from_utf8(utf8_qualified_name.bytes_as_string_view()));
    auto fly_namespace = namespace_.map([](auto const& value) {
        auto utf8 = value.to_utf8();
        return MUST(FlyString::from_utf8(utf8.bytes_as_string_view()));
    });
    auto extracted_qualified_name_or_error = validate_and_extract(fly_namespace, fly_qualified_name, ValidationContext::Element);
    if (extracted_qualified_name_or_error.is_error())
        return validate_and_extract_error_to_dom_exception(extracted_qualified_name_or_error.release_error());
    auto extracted_qualified_name = extracted_qualified_name_or_error.release_value();

    // 2. Let registry and is be the result of flattening element creation options given options and this.
    auto [registry, is_value] = TRY(flatten_element_creation_options(options));

    // 3. Return the result of creating an element given document, localName, namespace, prefix, is, true, and registry.
    return TRY(DOM::create_element(*this, extracted_qualified_name.local_name(), extracted_qualified_name.namespace_(), extracted_qualified_name.prefix(), move(is_value), true, registry));
}

WebIDL::ExceptionOr<GC::Ref<Element>> Document::create_element_ns(Optional<Utf16String> const& namespace_, Utf16String const& qualified_name, Variant<Utf16String, ElementCreationOptions> const& options)
{
    ElementCreationOptions element_creation_options;
    options.visit(
        [&](Utf16String const& is) {
            element_creation_options.is = is;
        },
        [&](ElementCreationOptions const& options) {
            element_creation_options = options;
        });
    return create_element_ns(namespace_, qualified_name, element_creation_options);
}

WebIDL::ExceptionOr<GC::Ref<Element>> Document::create_element_ns(Optional<Utf16FlyString> const& namespace_, Utf16String const& qualified_name, Variant<Utf16String, ElementCreationOptions> const& options)
{
    auto namespace_utf16 = namespace_.map([](auto const& value) { return value.to_utf16_string(); });
    return create_element_ns(namespace_utf16, qualified_name, options);
}

GC::Ref<DocumentFragment> Document::create_document_fragment()
{
    return DocumentFragment::create(*this);
}

GC::Ref<Text> Document::create_text_node(Utf16String data)
{
    return Text::create(*this, move(data));
}

// https://dom.spec.whatwg.org/#dom-document-createcdatasection
WebIDL::ExceptionOr<GC::Ref<CDATASection>> Document::create_cdata_section(Utf16String data)
{
    // 1. If this is an HTML document, then throw a "NotSupportedError" DOMException.
    if (is_html_document())
        return WebIDL::NotSupportedError::create("This operation is not supported for HTML documents"_utf16);

    // 2. If data contains the string "]]>", then throw an "InvalidCharacterError" DOMException.
    if (data.contains("]]>"sv))
        return WebIDL::InvalidCharacterError::create("String may not contain ']]>'"_utf16);

    // 3. Return a new CDATASection node with its data set to data and node document set to this.
    return CDATASection::create(*this, move(data));
}

GC::Ref<Comment> Document::create_comment(Utf16String data)
{
    return Comment::create(*this, move(data));
}

// https://dom.spec.whatwg.org/#dom-document-createprocessinginstruction
WebIDL::ExceptionOr<GC::Ref<ProcessingInstruction>> Document::create_processing_instruction(Utf16FlyString const& target, Utf16String data)
{
    // 1. If target does not match the Name production, then throw an "InvalidCharacterError" DOMException.
    if (!is_valid_name(target))
        return WebIDL::InvalidCharacterError::create("Invalid character in target name."_utf16);

    // 2. If data contains the string "?>", then throw an "InvalidCharacterError" DOMException.
    if (data.contains("?>"sv))
        return WebIDL::InvalidCharacterError::create("String may not contain '?>'"_utf16);

    // 3. Return a new ProcessingInstruction node, with target set to target, data set to data, and node document set to this.
    return ProcessingInstruction::create(*this, move(data), target);
}

GC::Ref<Range> Document::create_range()
{
    return Range::create(*this);
}

// https://dom.spec.whatwg.org/#dom-document-createevent
WebIDL::ExceptionOr<GC::Ref<Event>> Document::create_event(Utf16FlyString const& interface)
{
    auto& realm = relevant_settings_object().realm();
    auto time_stamp = HighResolutionTime::current_high_resolution_time(realm.global_object());

    // NOTE: This is named event here, since we do step 5 and 6 as soon as possible for each case.
    // 1. Let constructor be null.
    GC::Ptr<Event> event;

    // 2. If interface is an ASCII case-insensitive match for any of the strings in the first column in the following table,
    //      then set constructor to the interface in the second column on the same row as the matching string:
    if (interface.equals_ignoring_ascii_case("beforeunloadevent"sv)) {
        event = HTML::BeforeUnloadEvent::create(Utf16FlyString {}, {}, time_stamp);
    } else if (interface.equals_ignoring_ascii_case("compositionevent"sv)) {
        event = UIEvents::CompositionEvent::create(String {}, {}, time_stamp);
    } else if (interface.equals_ignoring_ascii_case("customevent"sv)) {
        event = CustomEvent::create(FlyString {}, {}, time_stamp);
    } else if (interface.equals_ignoring_ascii_case("devicemotionevent"sv)) {
        event = Event::create(FlyString {}, time_stamp); // FIXME: Create DeviceMotionEvent
    } else if (interface.equals_ignoring_ascii_case("deviceorientationevent"sv)) {
        event = Event::create(FlyString {}, time_stamp); // FIXME: Create DeviceOrientationEvent
    } else if (interface.equals_ignoring_ascii_case("dragevent"sv)) {
        event = HTML::DragEvent::create(Utf16FlyString {}, {}, 0, 0, 0, 0, time_stamp);
    } else if (interface.equals_ignoring_ascii_case("event"sv)
        || interface.equals_ignoring_ascii_case("events"sv)) {
        event = Event::create(FlyString {}, time_stamp);
    } else if (interface.equals_ignoring_ascii_case("focusevent"sv)) {
        event = UIEvents::FocusEvent::create(FlyString {}, {}, time_stamp);
    } else if (interface.equals_ignoring_ascii_case("hashchangeevent"sv)) {
        event = HTML::HashChangeEvent::create(FlyString {}, {}, time_stamp);
    } else if (interface.equals_ignoring_ascii_case("htmlevents"sv)) {
        event = Event::create(FlyString {}, time_stamp);
    } else if (interface.equals_ignoring_ascii_case("keyboardevent"sv)) {
        event = UIEvents::KeyboardEvent::create(Utf16FlyString {}, {}, time_stamp);
    } else if (interface.equals_ignoring_ascii_case("messageevent"sv)) {
        event = HTML::MessageEvent::create(FlyString {}, time_stamp);
    } else if (interface.equals_ignoring_ascii_case("mouseevent"sv)
        || interface.equals_ignoring_ascii_case("mouseevents"sv)) {
        event = UIEvents::MouseEvent::create(FlyString {}, UIEvents::MouseEventOptions {}, 0, 0, 0, 0, time_stamp);
    } else if (interface.equals_ignoring_ascii_case("storageevent"sv)) {
        event = HTML::StorageEvent::create(Utf16FlyString {}, {}, time_stamp);
    } else if (interface.equals_ignoring_ascii_case("svgevents"sv)) {
        event = Event::create(FlyString {}, time_stamp);
    } else if (interface.equals_ignoring_ascii_case("textevent"sv)) {
        event = UIEvents::TextEvent::create(Utf16FlyString {}, time_stamp);
    } else if (interface.equals_ignoring_ascii_case("touchevent"sv)) {
        event = Event::create(FlyString {}, time_stamp); // FIXME: Create TouchEvent
    } else if (interface.equals_ignoring_ascii_case("uievent"sv)
        || interface.equals_ignoring_ascii_case("uievents"sv)) {
        event = UIEvents::UIEvent::create(FlyString {}, time_stamp);
    }

    // 3. If constructor is null, then throw a "NotSupportedError" DOMException.
    if (!event) {
        return WebIDL::NotSupportedError::create("No constructor for interface found"_utf16);
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

void Document::set_pending_parsing_blocking_svg_script(SVG::SVGScriptElement* script)
{
    m_pending_parsing_blocking_svg_script = script;
}

GC::Ref<SVG::SVGScriptElement> Document::take_pending_parsing_blocking_svg_script(Badge<HTML::HTMLParser>)
{
    VERIFY(m_pending_parsing_blocking_svg_script);
    auto script = m_pending_parsing_blocking_svg_script;
    m_pending_parsing_blocking_svg_script = nullptr;
    return *script;
}

void Document::add_script_to_execute_when_parsing_has_finished(Badge<HTML::HTMLScriptElement>, HTML::HTMLScriptElement& script)
{
    m_scripts_to_execute_when_parsing_has_finished.append(script);
}

// https://dom.spec.whatwg.org/#dom-document-importnode
WebIDL::ExceptionOr<GC::Ref<Node>> Document::import_node(GC::Ref<Node> node, ImportNodeOptions const& options)
{
    // 1. If node is a document or shadow root, then throw a "NotSupportedError" DOMException.
    if (is<Document>(*node) || is<ShadowRoot>(*node))
        return WebIDL::NotSupportedError::create("Cannot import a document or shadow root."_utf16);

    // 2. Let subtree be the negation of options["selfOnly"].
    bool subtree = !options.self_only;

    // 3. Let registry be options["customElementRegistry"] if it exists; otherwise null.
    auto registry = options.custom_element_registry;

    // 4. If registry’s is scoped is false and registry is not this’s custom element registry, then throw a
    //    "NotSupportedError" DOMException.
    if (registry && !registry->is_scoped() && registry != custom_element_registry())
        return WebIDL::NotSupportedError::create("'customElementRegistry' in ImportNodeOptions must either be scoped or the document's custom element registry."_utf16);

    // 5. If registry is null, then set registry to the result of looking up a custom element registry given this.
    if (!registry)
        registry = custom_element_registry();

    // 6. Return the result of cloning a node given node with document set to this, subtree set to subtree, and
    //   fallbackRegistry set to registry.
    return node->clone_node(this, subtree, nullptr, registry);
}

WebIDL::ExceptionOr<GC::Ref<Node>> Document::import_node(GC::Ref<Node> node, Variant<bool, ImportNodeOptions> const& options)
{
    auto import_node_options = options.visit(
        [](bool value) -> ImportNodeOptions {
            return {
                .custom_element_registry = nullptr,
                .self_only = !value,
            };
        },
        [](ImportNodeOptions const& options) {
            return options;
        });
    return import_node(node, import_node_options);
}

// https://dom.spec.whatwg.org/#concept-node-adopt
void Document::adopt_node_steps(Node& node)
{
    // 1. Let oldDocument be node’s node document.
    auto& old_document = node.document();

    // 2. If node’s parent is non-null, then remove node.
    if (node.parent())
        node.remove();

    // 3. If document is not oldDocument, then:
    if (&old_document != this) {
        Vector<GC::Ref<ShadowRoot>> shadow_roots_with_adopted_sheets;

        // A sheet adopted into a shadow root travels with it, and its identity in the style engine
        // belongs to the document it is leaving. Give it up here, while that document is still the
        // one the sheet answers to, and take a new one below.
        node.for_each_shadow_including_inclusive_descendant([&](DOM::Node& inclusive_descendant) {
            if (auto* shadow_root = as_if<ShadowRoot>(inclusive_descendant)) {
                shadow_roots_with_adopted_sheets.append(*shadow_root);
                for_each_adopted_style_sheet(AdoptedStyleSheetsAccess::adopted_style_sheets(*shadow_root), [&](CSS::CSSStyleSheet& sheet) {
                    CSS::record_stylesheet_detached(sheet, *shadow_root);
                });
            }
            return TraversalDecision::Continue;
        });

        // 1. For each inclusiveDescendant in node’s shadow-including inclusive descendants:
        node.for_each_shadow_including_inclusive_descendant([&](DOM::Node& inclusive_descendant) {
            // 1. Set inclusiveDescendant’s node document to document.
            inclusive_descendant.set_document(Badge<Document> {}, *this);

            // 2. If inclusiveDescendant is a shadow root and if any of the following are true:
            //    - inclusiveDescendant’s custom element registry is null and inclusiveDescendant’s keep custom element
            //      registry null is false; or
            //    - inclusiveDescendant’s custom element registry is a global custom element registry,
            //    then set inclusiveDescendant’s custom element registry to document’s effective global custom element
            //    registry.
            if (auto* shadow_root = as_if<ShadowRoot>(inclusive_descendant); shadow_root
                && ((shadow_root->custom_element_registry() == nullptr && !shadow_root->keep_custom_element_registry_null())
                    || HTML::is_a_global_custom_element_registry(shadow_root->custom_element_registry()))) {

                shadow_root->set_custom_element_registry(effective_global_custom_element_registry());

            }

            // 3. Otherwise, if inclusiveDescendant is an element:
            else if (auto* element = as_if<Element>(inclusive_descendant)) {
                // 1. Set the node document of each attribute in inclusiveDescendant’s attribute list to document.
                element->for_each_attribute([this](Attr& attribute) {
                    attribute.set_document(Badge<Document> {}, *this);
                });

                // 2. If inclusiveDescendant’s custom element registry is null or inclusiveDescendant’s custom element
                //    registry’s is scoped is false, then set inclusiveDescendant’s custom element registry to
                //    document’s effective global custom element registry.
                if (!element->custom_element_registry() || !element->custom_element_registry()->is_scoped()) {
                    element->set_custom_element_registry(effective_global_custom_element_registry());
                }
            }

            return TraversalDecision::Continue;
        });

        // 2. For each inclusiveDescendant in node’s shadow-including inclusive descendants that is custom, in
        //    shadow-including tree order:
        //    enqueue a custom element callback reaction with inclusiveDescendant, callback name "adoptedCallback", and
        //    « oldDocument, document ».
        node.for_each_shadow_including_inclusive_descendant([&](DOM::Node& inclusive_descendant) {
            if (auto* element = as_if<Element>(inclusive_descendant); element && element->is_custom())
                element->enqueue_an_adopted_callback_reaction(old_document, *this);

            return TraversalDecision::Continue;
        });

        // 3. For each inclusiveDescendant in node’s shadow-including inclusive descendants, in shadow-including tree
        //     order:
        //    run the adopting steps with inclusiveDescendant and oldDocument.
        node.for_each_shadow_including_inclusive_descendant([&](auto& inclusive_descendant) {
            inclusive_descendant.adopted_from(old_document);
            return TraversalDecision::Continue;
        });

        // The engine the adopted sheets were attached to belongs to the document they left;
        // attaching them again is what tells this one that they decide here. Do this after the
        // adopting steps have minted the shadow root's identities for its new document.
        for (auto shadow_root : shadow_roots_with_adopted_sheets) {
            for_each_adopted_style_sheet(AdoptedStyleSheetsAccess::adopted_style_sheets(shadow_root), [&](CSS::CSSStyleSheet& sheet) {
                CSS::record_stylesheet_attached(sheet, shadow_root, nullptr);
                CSS::Invalidation::invalidate_style_after_adopting_style_sheet(shadow_root, sheet);
            });
        }

        // AD-HOC: Transfer NodeIterators rooted at `node` from old_document to this document.
        Vector<NodeIterator&> node_iterators_to_transfer;
        for (auto node_iterator : old_document.m_node_iterators) {
            if (node_iterator->root().ptr() == &node)
                node_iterators_to_transfer.append(*node_iterator);
        }

        for (auto& node_iterator : node_iterators_to_transfer) {
            old_document.m_node_iterators.remove(&node_iterator);
            m_node_iterators.set(&node_iterator);
        }

        // AD-HOC: Live ranges are registered with the document their boundary points belong to. The removal in step 2
        //         only clamps ranges anchored in node's own tree, so ranges anchored in a parentless node's tree cross
        //         into this document with their boundaries intact and must be re-registered here.
        if (!old_document.live_ranges().is_empty()) {
            Vector<GC::Ref<Range>> ranges_to_transfer;
            for (auto& range : old_document.live_ranges()) {
                if (&range.start_container()->document() == this)
                    ranges_to_transfer.append(range);
            }
            for (auto range : ranges_to_transfer)
                range->update_owner_document({});
        }

        // AD-HOC: A parentless node leaves oldDocument without any mutation observable through oldDocument's
        //         dom_tree_version. Bump it so that caches keyed on that version can't serve stale results for this
        //         node if it is later adopted back after being mutated under another document.
        old_document.bump_dom_tree_version();
    }
}

// https://dom.spec.whatwg.org/#dom-document-adoptnode
WebIDL::ExceptionOr<GC::Ref<Node>> Document::adopt_node(GC::Ref<Node> node)
{
    // 1. If node is a document, then throw a "NotSupportedError" DOMException.
    if (is<Document>(*node))
        return WebIDL::NotSupportedError::create("Cannot adopt a document into a document"_utf16);

    // 2. If node is a shadow root, then throw a "HierarchyRequestError" DOMException.
    if (is<ShadowRoot>(*node))
        return WebIDL::HierarchyRequestError::create("Cannot adopt a shadow root into a document"_utf16);

    adopt_node_steps(*node);

    // 4. Return node.
    return node;
}

DocumentType const* Document::doctype() const
{
    return first_child_of_type<DocumentType>();
}

Utf16FlyString Document::compat_mode() const
{
    if (m_quirks_mode == QuirksMode::Yes)
        return "BackCompat"_utf16_fly_string;

    return "CSS1Compat"_utf16_fly_string;
}

void Document::update_active_element()
{
    set_active_element(calculate_active_element(*this));
}

void Document::set_focused_area(GC::Ptr<Node> node, InvalidateFocusPseudoClasses invalidate_focus_pseudo_classes)
{
    if (m_focused_area == node)
        return;

    GC::Ptr old_focused_area = m_focused_area;

    if (auto* old_focused_element = as_if<Element>(old_focused_area.ptr()))
        old_focused_element->did_lose_focus();

    if (invalidate_focus_pseudo_classes == InvalidateFocusPseudoClasses::Yes) {
        CSS::Invalidation::invalidate_style_after_pseudo_class_state_change(CSS::PseudoClass::Focus, old_focused_area, node);
        CSS::Invalidation::invalidate_style_after_pseudo_class_state_change(CSS::PseudoClass::FocusWithin, old_focused_area, node);
        CSS::Invalidation::invalidate_style_after_pseudo_class_state_change(CSS::PseudoClass::FocusVisible, old_focused_area, node);
    }

    m_focused_area = node;

    auto* new_focused_element = as_if<Element>(node.ptr());
    if (new_focused_element)
        new_focused_element->did_receive_focus();

    reset_cursor_blink_cycle();

    set_needs_repaint();

    update_active_element();
}

Element const* Document::active_element() const
{
    return m_active_element ? m_active_element.ptr() : body();
}

void Document::set_active_element(GC::Ptr<Element> element)
{
    if (m_active_element == element)
        return;

    m_active_element = element;

    set_needs_repaint();
}

void Document::set_target_element(GC::Ptr<Element> element)
{
    if (m_target_element == element)
        return;

    GC::Ptr<Element> old_target_element = move(m_target_element);

    CSS::Invalidation::invalidate_style_after_pseudo_class_state_change(CSS::PseudoClass::Target, old_target_element, element);

    m_target_element = element;

    set_needs_repaint();
}

// https://html.spec.whatwg.org/multipage/interaction.html#flush-autofocus-candidates
void Document::flush_autofocus_candidates()
{
    // 1. If topDocument's autofocus processed flag is true, then return.
    if (m_autofocus_processed_flag)
        return;

    // 2. Let candidates be topDocument's autofocus candidates.
    auto& candidates = m_autofocus_candidates;

    // 3. If candidates is empty, then return.
    if (candidates.is_empty())
        return;

    // 4. If topDocument's focused area is not topDocument itself, or topDocument has non-null target element, then:
    if ((m_focused_area && m_focused_area.ptr() != this) || m_target_element) {
        // 1. Empty candidates.
        candidates.clear();

        // 2. Set topDocument's autofocus processed flag to true.
        m_autofocus_processed_flag = true;

        // 3. Return.
        return;
    }

    // 5. While candidates is not empty:
    while (!candidates.is_empty()) {
        // 1. Let element be candidates[0].
        GC::Ref<Element> element = candidates.first();

        // 2. Let doc be element's node document.
        auto& doc = element->document();

        // 3. If doc is not fully active, then remove element from candidates, and continue.
        if (!doc.is_fully_active()) {
            candidates.take_first();
            continue;
        }

        // 4. If doc's node navigable's top-level traversable is not the same as topDocument's node navigable, then
        //    remove element from candidates, and continue.
        auto doc_navigable = doc.navigable();
        if (!doc_navigable || doc_navigable->top_level_traversable() != navigable()) {
            candidates.take_first();
            continue;
        }

        // 5. If doc's script-blocking style sheet set is not empty, then return.
        if (!doc.script_blocking_style_sheet_set().is_empty())
            return;

        // 6. Remove element from candidates.
        candidates.take_first();

        // 7. Let inclusiveAncestorDocuments be a list consisting of the active document of doc's inclusive ancestor navigables.
        GC::RootVector<GC::Ref<Document>> inclusive_ancestor_documents;
        inclusive_ancestor_documents.append(doc);
        auto parent_navigable = doc_navigable->parent();
        auto* ancestor_navigable = parent_navigable ? &as<HTML::LocalNavigable>(*parent_navigable) : nullptr;
        while (ancestor_navigable) {
            if (auto active = ancestor_navigable->active_document())
                inclusive_ancestor_documents.append(*active);
            parent_navigable = ancestor_navigable->parent();
            ancestor_navigable = parent_navigable ? &as<HTML::LocalNavigable>(*parent_navigable) : nullptr;
        }

        // 8. If any Document in inclusiveAncestorDocuments has non-null target element, then continue.
        auto any_ancestor_has_target = false;
        for (auto& ancestor : inclusive_ancestor_documents) {
            if (ancestor->target_element()) {
                any_ancestor_has_target = true;
                break;
            }
        }
        if (any_ancestor_has_target)
            continue;

        // 9. Let target be element.
        GC::Ptr<Element> target = element;

        // FIXME: 10. If target is not a focusable area, then set target to the result of getting the
        //            focusable area for target.
        // AD-HOC: We don't implement "get the focusable area" so for now, treat unconnected and non-focusable elements
        //         as having no focusable area.
        if (!target->is_connected() || !target->is_focusable())
            target = nullptr;

        // 11. If target is not null, then:
        if (target) {
            // 1. Empty candidates.
            candidates.clear();

            // 2. Set topDocument's autofocus processed flag to true.
            m_autofocus_processed_flag = true;

            // 3. Run the focusing steps for target.
            HTML::run_focusing_steps(target.ptr());
        }
    }
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
    auto fragment_as_utf16 = utf16_string_from_url_ascii(*fragment);
    auto* potential_indicated_element = find_a_potential_indicated_element(fragment_as_utf16);

    // 4. If potentialIndicatedElement is not null, then return potentialIndicatedElement.
    if (potential_indicated_element)
        return potential_indicated_element;

    // 5. Let fragmentBytes be the result of percent-decoding fragment.
    // 6. Let decodedFragment be the result of running UTF-8 decode without BOM on fragmentBytes.
    auto decoded_fragment = Utf16String::from_utf8_with_replacement_character(URL::percent_decode(*fragment), Utf16String::WithBOMHandling::No);

    // 7. Set potentialIndicatedElement to the result of finding a potential indicated element given document and decodedFragment.
    potential_indicated_element = find_a_potential_indicated_element(decoded_fragment);

    // 8. If potentialIndicatedElement is not null, then return potentialIndicatedElement.
    if (potential_indicated_element)
        return potential_indicated_element;

    // 9. If decodedFragment is an ASCII case-insensitive match for the string top, then return the top of the document.
    if (decoded_fragment.utf16_view().equals_ignoring_ascii_case(u"top"sv))
        return Document::TopOfTheDocument {};

    // 10. Return null.
    return nullptr;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#find-a-potential-indicated-element
Element* Document::find_a_potential_indicated_element(Utf16View fragment) const
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
        if (element.name().has_value() && element.name()->view() == fragment) {
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
        } else if (transition->current_time()->value < 0) {
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

    auto dispatch_event = [&](Utf16FlyString const& type, Interval interval) {
        // The target for a transition event is the transition’s owning element. If there is no owning element,
        // no transition events are dispatched.
        if (!transition->effect() || !transition->owning_element().has_value())
            return;

        auto effect = transition->effect();

        Animations::TimeValue elapsed_time = [&]() {
            if (interval == Interval::Start)
                return max(min(-effect->start_delay(), effect->active_duration()), Animations::TimeValue::create_zero(transition->timeline()));
            if (interval == Interval::End)
                return max(min(transition->associated_effect_end() - effect->start_delay(), effect->active_duration()), Animations::TimeValue::create_zero(transition->timeline()));
            if (interval == Interval::ActiveTime) {
                // The active time of the animation at the moment it was canceled calculated using a fill mode of both.
                // FIXME: Compute this properly.
                return Animations::TimeValue::create_zero(transition->timeline());
            }
            VERIFY_NOT_REACHED();
        }();

        double elapsed_time_output;

        switch (elapsed_time.type) {
        case Animations::TimeValue::Type::Milliseconds:
            elapsed_time_output = elapsed_time.value / 1000;
            break;
        case Animations::TimeValue::Type::Percentage:
            // FIXME: The spec doesn't specify how to handle this case
            elapsed_time_output = 0;
            break;
        }

        auto timeline = transition->timeline();

        append_pending_animation_event({
            .event = CSS::TransitionEvent::create(
                type,
                String { transition->transition_property().view().to_utf8_but_should_be_ported_to_utf16() },
                elapsed_time_output,
                transition->owning_element()->pseudo_element().map([](auto it) {
                                                                  return MUST(String::formatted("::{}", CSS::pseudo_element_name(it)));
                                                              })
                    .value_or({}),
                HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(transition->owning_element()->element().document())),
                transition),
            .animation = transition,
            .target = transition->owning_element()->element(),
            .scheduled_event_time = timeline && timeline->can_convert_a_timeline_time_to_an_origin_relative_time()
                ? timeline->convert_a_timeline_time_to_an_origin_relative_time(timeline->current_time())
                : Optional<double> {},
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
            dispatch_event(HTML::EventNames::transitioncancel, Interval::ActiveTime);
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

    auto previous_phase = effect->previous_phase();
    auto current_phase = effect->phase();
    auto current_iteration = effect->current_iteration().value_or(0.0);

    auto owning_element = css_animation.owning_element();

    if (!owning_element.has_value())
        return;

    auto dispatch_event = [&](Utf16FlyString const& name, Animations::TimeValue elapsed_time) {
        double elapsed_time_output;
        switch (elapsed_time.type) {
        case Animations::TimeValue::Type::Milliseconds:
            elapsed_time_output = elapsed_time.value / 1000;
            break;
        case Animations::TimeValue::Type::Percentage:
            // FIXME: The spec doesn't specify how to handle this case
            elapsed_time_output = 0;
            break;
        }

        auto timeline = animation->timeline();

        Bindings::AnimationEventInit event_init {};
        event_init.bubbles = true;
        event_init.animation_name = css_animation.animation_name().to_utf16_string();
        event_init.elapsed_time = elapsed_time_output;
        if (auto pseudo_element = owning_element->pseudo_element().map([](auto it) {
                return Utf16String::formatted("::{}", CSS::pseudo_element_name(it));
            });
            pseudo_element.has_value()) {
            event_init.pseudo_element = pseudo_element.release_value();
        }
        event_init.animation = css_animation;

        append_pending_animation_event({
            .event = CSS::AnimationEvent::create(
                name,
                event_init,
                HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(owning_element->element()))),
            .animation = css_animation,
            .target = owning_element->element(),
            .scheduled_event_time = timeline && timeline->can_convert_a_timeline_time_to_an_origin_relative_time()
                ? timeline->convert_a_timeline_time_to_an_origin_relative_time(timeline->current_time())
                : Optional<double> {},
        });
    };

    // For calculating the elapsedTime of each event, the following definitions are used:

    // - interval start = max(min(-start delay, active duration), 0)
    auto interval_start = max(min(-effect->start_delay(), effect->active_duration()), Animations::TimeValue::create_zero(animation->timeline()));

    // - interval end = max(min(associated effect end - start delay, active duration), 0)
    auto interval_end = max(min(effect->end_time() - effect->start_delay(), effect->active_duration()), Animations::TimeValue::create_zero(animation->timeline()));

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
                auto elapsed_time = effect->iteration_duration() * (iteration_boundary - effect->iteration_start());

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
        auto cancel_time = animation->release_saved_cancel_time().value_or(Animations::TimeValue::create_zero(animation->timeline()));
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

        // FIXME: 4. Run the ancestor revealing algorithm on target.

        // 5. Scroll target into view, with behavior set to "auto", block set to "start", and inline set to "nearest". [CSSOMVIEW]
        Element::ScrollIntoViewOptions scroll_options;
        scroll_options.block = Element::ScrollLogicalPosition::Start;
        scroll_options.inline_ = Element::ScrollLogicalPosition::Nearest;
        target->scroll_into_view(scroll_options, nullptr);

        // 6. Run the focusing steps for target, with the Document's viewport as the fallback target.
        HTML::run_focusing_steps(target, this);

        // FIXME: 7. Move the sequential focus navigation starting point to target.
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
        navigable->perform_scroll_of_viewport_scrolling_box({ 0, 0 });
}

Utf16FlyString Document::ready_state() const
{
    switch (m_readiness) {
    case HTML::DocumentReadyState::Loading:
        return "loading"_utf16_fly_string;
    case HTML::DocumentReadyState::Interactive:
        return "interactive"_utf16_fly_string;
    case HTML::DocumentReadyState::Complete:
        return "complete"_utf16_fly_string;
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

    if (readiness_value != HTML::DocumentReadyState::Loading)
        page().client().request_frame();

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
    dispatch_event(Event::create(
        HTML::EventNames::readystatechange,
        HighResolutionTime::current_high_resolution_time(relevant_global_object(*this))));

    if (readiness_value == HTML::DocumentReadyState::Complete) {
        auto navigable = this->navigable();
        if (navigable)
            navigable->restore_pending_persisted_state_for_completed_document(*this);
        if (navigable && navigable->is_traversable()) {
            if (!is_decoded_svg()) {
                HTML::HTMLLinkElement::load_fallback_favicon_if_needed(*this);
            }
            navigable->traversable_navigable()->page().client().page_did_finish_loading(m_navigation_id, url());
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
Utf16String Document::last_modified() const
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

    if (m_last_modified.has_value()) {
        auto last_modified = MUST(m_last_modified.value().to_string(format_string));
        return Utf16String::from_ascii_without_validation(last_modified.bytes());
    }

    auto now = MUST(AK::UnixDateTime::now().to_string(format_string));
    return Utf16String::from_ascii_without_validation(now.bytes());
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

    return m_window.ptr();
}

// https://html.spec.whatwg.org/multipage/document-lifecycle.html#completely-loaded
bool Document::is_completely_loaded() const
{
    return m_completely_loaded_time.has_value();
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#completely-finish-loading
void Document::completely_finish_loading()
{
    m_ongoing_navigation_fetch_controller = nullptr;

    // 2. Set document's completely loaded time to the current time.
    // AD-HOC: Set this unconditionally, even if the document has no navigable yet.
    //         In the async state machine, documents created during populate may complete
    //         loading before they are activated. The timestamp must still be recorded so
    //         that activate_history_entry can detect this and re-trigger the container
    //         notification steps.
    if (!m_completely_loaded_time.has_value())
        m_completely_loaded_time = AK::UnixDateTime::now();

    auto navigable = this->navigable();
    if (!navigable) {
        m_completely_loaded_deferred = true;
        return;
    }
    m_completely_loaded_deferred = false;

    ScopeGuard notify_observers = [this] {
        notify_each_document_observer([&](auto const& document_observer) {
            return document_observer.document_completely_loaded();
        });
    };

    // 1. Assert: document's browsing context is non-null.
    VERIFY(browsing_context());

    // NOTE: See the end of shared_declarative_refresh_steps.
    if (m_active_refresh_timer)
        m_active_refresh_timer->start();

    // 3. Let container be document's browsing context's container.
    if (!navigable->container())
        return;

    auto container = GC::make_root(navigable->container());

    // 4. If container is an iframe element, then queue an element task on the DOM manipulation task source given container to run the iframe load event steps given container.
    if (container && is<HTML::HTMLIFrameElement>(*container)) {
        container->queue_an_element_task(HTML::Task::Source::DOMManipulation, [container] {
            run_iframe_load_event_steps(static_cast<HTML::HTMLIFrameElement&>(*container));
        });
    }
    // 5. Otherwise, if container is non-null, then queue an element task on the DOM manipulation task source given container to fire an event named load at container.
    else if (container) {
        container->queue_an_element_task(HTML::Task::Source::DOMManipulation, [container] {
            container->dispatch_event(DOM::Event::create(HTML::EventNames::load, HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(*container))));
        });
    }

    // AD-HOC: Finishing a child document can unblock its parent's load-event-delay phase, so wake the parent parser end
    //         state after queueing the container's load event.
    container->document().schedule_html_parser_end_check();
}

// https://html.spec.whatwg.org/multipage/dom.html#dom-document-cookie
WebIDL::ExceptionOr<Utf16String> Document::cookie()
{
    // On getting, if the document is a cookie-averse Document object, then the user agent must return the empty string.
    if (is_cookie_averse())
        return Utf16String {};

    // Otherwise, if the Document's origin is an opaque origin, the user agent must throw a "SecurityError" DOMException.
    if (origin().is_opaque())
        return WebIDL::SecurityError::create("Document origin is opaque"_utf16);

    // Otherwise, the user agent must return the cookie-string for the document's URL for a "non-HTTP" API, decoded using
    // UTF-8 decode without BOM.
    if (m_cookie_version_index.has_value()) {
        if (m_cookie_version == page().client().page_did_request_document_cookie_version(*m_cookie_version_index))
            return m_cookie;
    }

    auto [cookie_version, cookie] = page().client().page_did_request_cookie(m_url, HTTP::Cookie::Source::NonHttp);

    if (cookie_version.has_value()) {
        m_cookie_version = *cookie_version;
        m_cookie = Utf16String::from_utf8(cookie);
        return m_cookie;
    }

    return Utf16String::from_utf8(cookie);
}

// https://html.spec.whatwg.org/multipage/dom.html#dom-document-cookie
WebIDL::ExceptionOr<void> Document::set_cookie(Utf16View cookie_string)
{
    // On setting, if the document is a cookie-averse Document object, then the user agent must do nothing.
    if (is_cookie_averse())
        return {};

    // Otherwise, if the Document's origin is an opaque origin, the user agent must throw a "SecurityError" DOMException.
    if (origin().is_opaque())
        return WebIDL::SecurityError::create("Document origin is opaque"_utf16);

    // Otherwise, the user agent must act as it would when receiving a set-cookie-string for the document's URL via a
    // "non-HTTP" API, consisting of the new value encoded as UTF-8.
    auto cookie_string_utf8 = TRY_OR_THROW_OOM(vm(), cookie_string.to_utf8());
    if (auto cookie = HTTP::Cookie::parse_cookie(url(), cookie_string_utf8); cookie.has_value()) {
        page().client().page_did_set_cookie(m_url, cookie.value(), HTTP::Cookie::Source::NonHttp);
        reset_cookie_version();
    }

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
    if (!url().scheme().is_one_of("http"sv, "https"sv))
        return true;

    return false;
}

Utf16String Document::fg_color() const
{
    if (auto* body_element = body(); body_element && !is<HTML::HTMLFrameSetElement>(*body_element))
        return body_element->get_attribute_value(HTML::AttributeNames::text);
    return {};
}

void Document::set_fg_color(Utf16View value)
{
    if (auto* body_element = body(); body_element && !is<HTML::HTMLFrameSetElement>(*body_element))
        body_element->set_attribute_value(HTML::AttributeNames::text, value);
}

Utf16String Document::link_color() const
{
    if (auto* body_element = body(); body_element && !is<HTML::HTMLFrameSetElement>(*body_element))
        return body_element->get_attribute_value(HTML::AttributeNames::link);
    return {};
}

void Document::set_link_color(Utf16View value)
{
    if (auto* body_element = body(); body_element && !is<HTML::HTMLFrameSetElement>(*body_element))
        body_element->set_attribute_value(HTML::AttributeNames::link, value);
}

Utf16String Document::vlink_color() const
{
    if (auto* body_element = body(); body_element && !is<HTML::HTMLFrameSetElement>(*body_element))
        return body_element->get_attribute_value(HTML::AttributeNames::vlink);
    return {};
}

void Document::set_vlink_color(Utf16View value)
{
    if (auto* body_element = body(); body_element && !is<HTML::HTMLFrameSetElement>(*body_element))
        body_element->set_attribute_value(HTML::AttributeNames::vlink, value);
}

Utf16String Document::alink_color() const
{
    if (auto* body_element = body(); body_element && !is<HTML::HTMLFrameSetElement>(*body_element))
        return body_element->get_attribute_value(HTML::AttributeNames::alink);
    return {};
}

void Document::set_alink_color(Utf16View value)
{
    if (auto* body_element = body(); body_element && !is<HTML::HTMLFrameSetElement>(*body_element))
        body_element->set_attribute_value(HTML::AttributeNames::alink, value);
}

Utf16String Document::bg_color() const
{
    if (auto* body_element = body(); body_element && !is<HTML::HTMLFrameSetElement>(*body_element))
        return body_element->get_attribute_value(HTML::AttributeNames::bgcolor);
    return {};
}

void Document::set_bg_color(Utf16View value)
{
    if (auto* body_element = body(); body_element && !is<HTML::HTMLFrameSetElement>(*body_element))
        body_element->set_attribute_value(HTML::AttributeNames::bgcolor, value);
}

Utf16String Document::dump_dom_tree_as_json() const
{
    const_cast<Document&>(*this).update_layout(UpdateLayoutReason::InspectDOMTree);

    Utf16StringBuilder builder;
    auto json = MUST(JsonObjectSerializer<>::try_create(builder));
    serialize_tree_as_json(json);

    MUST(json.finish());
    return builder.to_string();
}

// https://html.spec.whatwg.org/multipage/semantics.html#has-a-style-sheet-that-is-blocking-scripts
bool Document::has_a_style_sheet_that_is_blocking_scripts() const
{
    // 1. If document's script-blocking style sheet set is not empty, then return true.
    // INTEROP: The spec goes on to also return true when the container document's script-blocking style sheet set is
    //          non-empty (steps 2-4). No engine implements that fully: Blink and WebKit only ever consult the
    //          document's own set, and Gecko only consults ancestor documents for parser-blocking scripts, not for
    //          deferred or module scripts. Blocking child document scripts on the container document's style sheets
    //          makes iframes needlessly wait for the embedding page's CSS, so match Blink and WebKit by only
    //          considering our own set.
    return !m_script_blocking_style_sheet_set.is_empty();
}

void Document::set_referrer(Utf16String referrer)
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
    if (container_document && container_document.ptr() != this && container_document->is_fully_active())
        return true;

    return false;
}

bool Document::is_active() const
{
    auto navigable = this->navigable();
    return navigable && navigable->active_document().ptr() == this;
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
Utf16FlyString Document::visibility_state() const
{
    switch (m_visibility_state) {
    case HTML::VisibilityState::Hidden:
        return "hidden"_utf16_fly_string;
    case HTML::VisibilityState::Visible:
        return "visible"_utf16_fly_string;
    }
    VERIFY_NOT_REACHED();
}

// https://html.spec.whatwg.org/multipage/interaction.html#update-the-visibility-state
void Document::update_the_visibility_state(HTML::VisibilityState visibility_state)
{
    // 1. If document's visibility state equals visibilityState, then return.
    if (m_visibility_state == visibility_state)
        return;

    // 2. Set document's visibility state to visibilityState.
    m_visibility_state = visibility_state;

    // FIXME: 3. Queue a new VisibilityStateEntry whose visibility state is visibilityState and whose timestamp is the current
    //    high resolution time given document's relevant global object.

    // FIXME: 4. Run the screen orientation change steps with document.

    // 5. Run the view transition page visibility change steps with document.
    view_transition_page_visibility_change_steps();

    // 6. Run any page visibility change steps which may be defined in other specifications, with visibility state and
    //    document.
    notify_each_document_observer([&](auto const& document_observer) {
        return document_observer.document_visibility_state_observer();
    },
        m_visibility_state);

    // 7. Fire an event named visibilitychange at document, with its bubbles attribute initialized to true.
    auto event = DOM::Event::create(
        HTML::EventNames::visibilitychange,
        HighResolutionTime::current_high_resolution_time(relevant_global_object(*this)));
    event->set_bubbles(true);
    dispatch_event(event);

    if (m_visibility_state == HTML::VisibilityState::Visible) {
        flush_throttled_animation_style_update();
        page().client().request_frame();
    }
}

// https://drafts.csswg.org/cssom-view/#document-run-the-resize-steps
void Document::run_the_resize_steps()
{
    // 1. If doc’s viewport has had its width or height changed
    //    (e.g. as a result of the user resizing the browser window, or changing the page zoom scale factor,
    //    or an iframe element’s dimensions are changed) since the last time these steps were run,
    //    fire an event named resize at the Window object associated with doc.
    // 2. If the VisualViewport associated with doc has had its scale, width, or height properties changed
    //    since the last time these steps were run, fire an event named resize at the VisualViewport.

    auto viewport_size = viewport_rect().size().to_type<int>();
    auto& visual_viewport = *this->visual_viewport();
    VisualViewportState visual_viewport_state = { visual_viewport.scale(), { visual_viewport.width(), visual_viewport.height() } };
    bool is_initial_size = !m_last_viewport_size.has_value();

    bool viewport_size_changed = m_last_viewport_size != viewport_size;
    bool visual_viewport_state_changed = m_last_visual_viewport_state != visual_viewport_state;

    if (!viewport_size_changed && !visual_viewport_state_changed)
        return;

    m_last_viewport_size = viewport_size;
    m_last_visual_viewport_state = visual_viewport_state;

    if (!is_initial_size) {
        if (viewport_size_changed) {
            auto window_resize_event = DOM::Event::create(
                UIEvents::EventNames::resize,
                HighResolutionTime::current_high_resolution_time(relevant_global_object(*this)));
            window_resize_event->set_is_trusted(true);
            window()->dispatch_event(window_resize_event);
        }

        if (visual_viewport_state_changed) {
            auto visual_viewport_resize_event = DOM::Event::create(
                UIEvents::EventNames::resize,
                HighResolutionTime::current_high_resolution_time(relevant_global_object(*this)));
            visual_viewport_resize_event->set_is_trusted(true);
            visual_viewport.dispatch_event(visual_viewport_resize_event);
        }
    }
}

bool Document::append_pending_scroll_event(PendingScrollEvent event)
{
    if (m_pending_scroll_events.contains_slow(event))
        return false;
    m_pending_scroll_events.append(move(event));
    return true;
}

// https://drafts.csswg.org/cssom-view-1/#document-run-the-scroll-steps
void Document::run_the_scroll_steps()
{
    // AD-HOC: Process auto-scroll ticks before dispatching scroll events. This is tied to the rendering update to
    //         ensure exactly one auto-scroll tick per frame.
    if (auto navigable = this->navigable())
        navigable->event_handler().process_auto_scroll();

    // FIXME: 1. For each scrolling box box that was scrolled:
    //        ...

    // 2. For each item (target, type) in doc’s pending scroll events, in the order they were added to the list, run
    //    these substeps:
    auto pending_scroll_events = move(m_pending_scroll_events);
    for (auto const& [target, type] : pending_scroll_events) {
        // 1. If target is a Document, and type is "scroll" or "scrollend", fire an event named type that bubbles at target.
        if (is<Document>(*target) && (type == HTML::EventNames::scroll || type == HTML::EventNames::scrollend)) {
            auto event = DOM::Event::create(
                type,
                HighResolutionTime::current_high_resolution_time(relevant_global_object(*this)));
            event->set_bubbles(true);
            target->dispatch_event(event);
        }
        // FIXME: 2. Otherwise, if type is "scrollsnapchange", then:
        // FIXME: 3. Otherwise, if type is "scrollsnapchanging", then:
        // 4. Otherwise, fire an event named type at target.
        else {
            auto event = DOM::Event::create(
                type,
                HighResolutionTime::current_high_resolution_time(relevant_global_object(*this)));
            target->dispatch_event(event);
        }
    }

    // 3. Empty doc’s pending scroll events.
    // AD-HOC: We already emptied the scroll events by moving in step 2. This prevents us from removing new scroll
    //         events that were added while dispatching the old scroll events.
}

void Document::add_media_query_list(GC::Ref<CSS::MediaQueryList> media_query_list)
{
    m_media_query_lists.append(media_query_list);
    m_needs_media_query_list_evaluation = true;
}

// https://drafts.csswg.org/cssom-view/#evaluate-media-queries-and-report-changes
void Document::evaluate_media_queries_and_report_changes()
{
    if (!m_needs_media_query_list_evaluation && !m_needs_media_rule_evaluation)
        return;

    bool evaluate_media_query_lists = m_needs_media_query_list_evaluation;
    m_needs_media_query_list_evaluation = false;

    if (evaluate_media_query_lists) {
        // NOTE: Not in the spec, but we take this opportunity to prune null WeakPtrs.
        m_media_query_lists.remove_all_matching([](auto& it) {
            return !it;
        });

        // 1. For each MediaQueryList object target that has doc as its document,
        //    in the order they were created, oldest first, run these substeps:
        for (auto& media_query_list_ptr : m_media_query_lists) {
            // 1. If target’s matches state has changed since the last time these steps
            //    were run, fire an event at target using the MediaQueryListEvent constructor,
            //    with its type attribute initialized to change, its isTrusted attribute
            //    initialized to true, its media attribute initialized to target’s media,
            //    and its matches attribute initialized to target’s matches state.
            if (!media_query_list_ptr)
                continue;
            GC::Ptr<CSS::MediaQueryList> media_query_list = media_query_list_ptr.ptr();
            bool did_match = media_query_list->matches();
            bool now_matches = media_query_list->evaluate();

            auto did_change_internally = media_query_list->has_changed_state();
            media_query_list->set_has_changed_state(false);
            if (did_change_internally == true || did_match != now_matches) {
                auto event = CSS::MediaQueryListEvent::create(
                    HTML::EventNames::change,
                    media_query_list->media(),
                    now_matches,
                    HighResolutionTime::current_high_resolution_time(relevant_global_object(*this)));
                event->set_is_trusted(true);
                media_query_list->dispatch_event(*event);
            }
        }
    }

    // Also not in the spec, but this is as good a place as any to evaluate @media rules!
    if (m_needs_media_rule_evaluation)
        evaluate_media_rules();
}

void Document::evaluate_media_rules()
{
    m_needs_media_rule_evaluation = false;
    CSS::Invalidation::evaluate_media_rules_and_publish_conditions(*this);
}

DOMImplementation* Document::implementation()
{
    if (!m_implementation)
        m_implementation = DOMImplementation::create(*this);
    return m_implementation.ptr();
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-document-hasfocus
bool Document::has_focus_for_bindings() const
{
    // The Document hasFocus() method steps are to return the result of running the has focus steps given this.
    return has_focus();
}

// https://html.spec.whatwg.org/multipage/interaction.html#has-focus-steps
bool Document::has_focus() const
{
    // 1. If target's node navigable's top-level traversable does not have system focus, then return false.
    auto navigable = this->navigable();
    if (!navigable)
        return false;

    auto traversable = navigable->traversable_navigable();
    if (!traversable || !traversable->is_focused())
        return false;

    // 2. Let candidate be target's node navigable's top-level traversable's active document.
    auto candidate = traversable->active_document();

    // 3. While true:
    while (candidate) {
        // 3.1. If candidate is target, then return true.
        if (candidate.ptr() == this)
            return true;

        // 3.2. If the focused area of candidate is a navigable container with a non-null content navigable,
        //      then set candidate to the active document of that navigable container's content navigable.
        auto focused_area = candidate->focused_area();
        if (auto* navigable_container = as_if<HTML::NavigableContainer>(focused_area.ptr())) {
            if (auto content_navigable = navigable_container->content_navigable()) {
                candidate = as<HTML::LocalNavigable>(*content_navigable).active_document();
                continue;
            }
        }

        // 3.3. Otherwise, return false.
        return false;
    }

    return false;
}

// https://html.spec.whatwg.org/multipage/interaction.html#allow-focus-steps
bool Document::allow_focus() const
{
    // The allow focus steps, given a Document object target, are as follows:

    // 1. If target is allowed to use the "focus-without-user-activation" feature, then return true.
    if (is_allowed_to_use_feature(PolicyControlledFeature::FocusWithoutUserActivation))
        return true;

    // 2. If target's relevant global object has transient activation, then return true.
    if (HTML::relevant_window(*this).has_transient_activation())
        return true;

    // 3. Return false.
    return false;
}

void Document::set_parser(Badge<HTML::HTMLParser>, HTML::HTMLParser& parser)
{
    ++m_parser_generation;
    m_parser = parser;
}

void Document::detach_parser()
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
bool Document::is_valid_name(Utf16View const& name)
{
    if (name.is_empty())
        return false;
    auto it = name.begin();

    if (!is_valid_name_start_character(*it))
        return false;
    ++it;

    for (; it != name.end(); ++it) {
        if (!is_valid_name_character(*it))
            return false;
    }

    return true;
}

// https://dom.spec.whatwg.org/#dom-document-createnodeiterator
GC::Ref<NodeIterator> Document::create_node_iterator(Node& root, unsigned what_to_show, GC::Ptr<NodeFilter> filter)
{
    return NodeIterator::create(root, what_to_show, filter);
}

// https://dom.spec.whatwg.org/#dom-document-createtreewalker
GC::Ref<TreeWalker> Document::create_tree_walker(Node& root, unsigned what_to_show, GC::Ptr<NodeFilter> filter)
{
    return TreeWalker::create(root, what_to_show, filter);
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

void Document::attach_range(Badge<Range>, Range& range)
{
    VERIFY(!m_live_ranges.contains(range));
    m_live_ranges.append(range);
}

void Document::detach_range(Badge<Range>, Range& range)
{
    VERIFY(m_live_ranges.contains(range));
    m_live_ranges.remove(range);
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

void Document::register_svg_use_element(Badge<SVG::SVGUseElement>, SVG::SVGUseElement& use_element)
{
    m_svg_use_elements.append(use_element);
}

void Document::unregister_svg_use_element(Badge<SVG::SVGUseElement>, SVG::SVGUseElement& use_element)
{
    m_svg_use_elements.remove(use_element);
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

    schedule_html_parser_end_check();
}

void Document::increment_number_of_pending_style_sheet_requests(Badge<DocumentLoadEventDelayer>)
{
    ++m_number_of_pending_style_sheet_requests;
    page().client().request_frame();
}

void Document::decrement_number_of_pending_style_sheet_requests(Badge<DocumentLoadEventDelayer>)
{
    VERIFY(m_number_of_pending_style_sheet_requests);
    --m_number_of_pending_style_sheet_requests;
    page().client().request_frame();
}

void Document::set_html_parser_end_state(GC::Ptr<HTML::HTMLParserEndState> state)
{
    m_html_parser_end_state = state;
}

void Document::schedule_html_parser_end_check()
{
    if (m_html_parser_end_state)
        m_html_parser_end_state->schedule_progress_check();
    if (m_parser)
        m_parser->schedule_resume_check();

    // NB: Whatever unblocked this document's load event may also have been the last thing delaying the load event of
    //     an ancestor document, since NavigableContainer::currently_delays_the_load_event() considers everything that
    //     delays the load event of a container's active document. Wake ancestor parser end states so they notice.
    //     This implements the re-evaluation implied by "the end" step 8, which spins the event loop until there is
    //     nothing that delays the load event in the ancestor's document. Repeated wake-ups within one event loop
    //     turn coalesce into a single deferred progress check via HTMLParserEndState::m_check_pending.
    if (auto navigable = this->navigable()) {
        if (auto container = navigable->container())
            container->document().schedule_html_parser_end_check();
    }
}

void Document::remove_from_script_blocking_style_sheet_set(Element& element)
{
    // NB: Removing from the set can unblock this document's parser or parser end state, both of which wait for the
    //     set to become empty. Always schedule the wake-up here so no removal site can forget it.
    m_script_blocking_style_sheet_set.remove(element);
    schedule_html_parser_end_check();
}

void Document::set_ready_for_post_load_tasks(bool ready)
{
    m_ready_for_post_load_tasks = ready;
    if (ready) {
        if (auto navigable = this->navigable()) {
            // AD-HOC: Clear the navigation load event guard now that the document is ready.
            //         This guard was set in finalize_a_cross_document_navigation to prevent the parent's
            //         load event from firing while the about:blank was still the active document.
            navigable->clear_navigation_load_event_guard();

            if (auto container = navigable->container()) {
                container->document().schedule_html_parser_end_check();
            }
        }
    }
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

void Document::check_favicon_after_loading_link_resource()
{
    // https://html.spec.whatwg.org/multipage/links.html#rel-icon
    // NOTE: firefox also load favicons outside the head tag, which is against spec (see table 4.6.7)
    auto* head_element = head();
    if (!head_element)
        return;

    auto favicon_link_elements = HTMLCollection::create(*head_element, HTMLCollection::Scope::Descendants, [](Element const& element) {
        if (auto const* link_element = as_if<HTML::HTMLLinkElement>(element))
            return link_element->has_loaded_icon();
        return false; }, HTMLCollection::AttributeInvalidationType::None);

    if (favicon_link_elements->length() == 0) {
        dbgln_if(SPAM_DEBUG, "No favicon found to be used");
        return;
    }

    // If multiple icons are provided, the user agent must select the most appropriate icon according to the type,
    // media, and sizes attributes. If there are multiple equally appropriate icons, user agents must use the last one
    // declared in tree order at the time that the user agent collected the list of icons.
    RefPtr<Gfx::Bitmap const> largest_icon;

    for (auto i = favicon_link_elements->length(); i-- > 0;) {
        auto* link_element = static_cast<HTML::HTMLLinkElement*>(favicon_link_elements->item(i));
        if (link_element == m_active_element.ptr())
            return;

        // If the user agent tries to use an icon but that icon is determined, upon closer examination, to in fact be
        // inappropriate (e.g. because it uses an unsupported format), then the user agent must try the
        // next-most-appropriate icon as determined by the attributes.
        if (auto icon = link_element->load_favicon_if_window_is_active()) {
            if (!largest_icon || icon->size().area() > largest_icon->size().area()) {
                m_active_favicon = link_element;
                largest_icon = move(icon);
            }
        }
    }

    if (largest_icon) {
        if (auto navigable = this->navigable(); navigable && navigable->is_traversable())
            navigable->traversable_navigable()->page().client().page_did_change_favicon(*largest_icon);
    } else {
        dbgln_if(SPAM_DEBUG, "No favicon found to be used");
    }
}

void Document::set_window(HTML::Window& window)
{
    m_window = &window;
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
        m_history = HTML::History::create(*this);
    return *m_history;
}

GC::Ref<HTML::History> Document::history() const
{
    return const_cast<Document*>(this)->history();
}

// https://html.spec.whatwg.org/multipage/origin.html#dom-document-domain
Utf16String Document::domain() const
{
    // 1. Let effectiveDomain be this's origin's effective domain.
    auto effective_domain = origin().effective_domain();

    // 2. If effectiveDomain is null, then return the empty string.
    if (!effective_domain.has_value())
        return Utf16String {};

    // 3. Return effectiveDomain, serialized.
    return utf16_string_from_url_ascii(effective_domain->serialize());
}

// https://html.spec.whatwg.org/multipage/browsers.html#is-a-registrable-domain-suffix-of-or-is-equal-to
bool is_a_registrable_domain_suffix_of_or_is_equal_to(Utf16View host_suffix_string, URL::Host const& original_host)
{
    // 1. If hostSuffixString is the empty string, then return false.
    if (host_suffix_string.is_empty())
        return false;

    // 2. Let hostSuffix be the result of parsing hostSuffixString.
    auto host_suffix = URL::Parser::parse_host(host_suffix_string);

    // 3. If hostSuffix is failure, then return false.
    if (!host_suffix.has_value())
        return false;

    return is_a_registrable_domain_suffix_of_or_is_equal_to(host_suffix.value(), original_host);
}

static bool ends_with_dot_prefixed_suffix(Utf16View string, Utf16View suffix)
{
    if (string.length_in_code_units() <= suffix.length_in_code_units())
        return false;
    if (!string.ends_with(suffix))
        return false;
    return string.code_unit_at(string.length_in_code_units() - suffix.length_in_code_units() - 1) == '.';
}

bool is_a_registrable_domain_suffix_of_or_is_equal_to(URL::Host const& host_suffix, URL::Host const& original_host)
{
    // 4. If hostSuffix does not equal originalHost, then:
    if (host_suffix != original_host) {
        // 1. If hostSuffix or originalHost is not a domain, then return false.
        // NOTE: This excludes hosts that are IP addresses.
        if (!host_suffix.has<String>() || !original_host.has<String>())
            return false;
        auto const& host_suffix_string = host_suffix.get<String>();
        auto const& original_host_string = original_host.get<String>();

        // 2. If hostSuffix, prefixed by U+002E (.), does not match the end of originalHost, then return false.
        if (!ends_with_dot_prefixed_suffix(Utf16View { original_host_string.bytes_as_string_view() }, Utf16View { host_suffix_string.bytes_as_string_view() }))
            return false;

        // 3. If any of the following are true:
        //     * hostSuffix equals hostSuffix's public suffix; or
        //     * hostSuffix, prefixed by U+002E (.), matches the end of originalHost's public suffix,
        //    then return false. [URL]
        if (host_suffix_string == host_suffix.public_suffix())
            return false;

        auto original_host_public_suffix = original_host.public_suffix();
        VERIFY(original_host_public_suffix.has_value());

        if (ends_with_dot_prefixed_suffix(Utf16View { original_host_public_suffix->bytes_as_string_view() }, Utf16View { host_suffix_string.bytes_as_string_view() }))
            return false;

        // 4. Assert: originalHost's public suffix, prefixed by U+002E (.), matches the end of hostSuffix.
        VERIFY(ends_with_dot_prefixed_suffix(Utf16View { host_suffix_string.bytes_as_string_view() }, Utf16View { original_host_public_suffix->bytes_as_string_view() }));
    }

    // 5. Return true.
    return true;
}

// https://html.spec.whatwg.org/multipage/browsers.html#dom-document-domain
WebIDL::ExceptionOr<void> Document::set_domain(Utf16View domain)
{
    // 1. If this's browsing context is null, then throw a "SecurityError" DOMException.
    if (!m_browsing_context)
        return WebIDL::SecurityError::create("Document.domain setter requires a browsing context"_utf16);

    // 2. If this's active sandboxing flag set has its sandboxed document.domain browsing context flag set, then throw a "SecurityError" DOMException.
    if (has_flag(active_sandboxing_flag_set(), HTML::SandboxingFlagSet::SandboxedDocumentDomain))
        return WebIDL::SecurityError::create("Document.domain setter is sandboxed"_utf16);

    // 3. Let effectiveDomain be this's origin's effective domain.
    auto effective_domain = origin().effective_domain();

    // 4. If effectiveDomain is null, then throw a "SecurityError" DOMException.
    if (!effective_domain.has_value())
        return WebIDL::SecurityError::create("Document.domain setter called on a Document with a null effective domain"_utf16);

    // 5. If the given value is not a registrable domain suffix of and is not equal to effectiveDomain, then throw a "SecurityError" DOMException.
    if (!is_a_registrable_domain_suffix_of_or_is_equal_to(domain, effective_domain.value()))
        return WebIDL::SecurityError::create("Document.domain setter called for an invalid domain"_utf16);

    // FIXME: 6. If the surrounding agent's agent cluster's is origin-keyed is true, then return.

    // FIXME: 7. Set this's origin's domain to the result of parsing the given value.

    dbgln("(STUBBED) Document::set_domain(domain='{}')", domain);
    return {};
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
    if (!m_policy_container) {
        m_policy_container = GC::Heap::the().allocate<HTML::PolicyContainer>(GC::Heap::the());
    }
    return *m_policy_container;
}

void Document::set_policy_container(GC::Ref<HTML::PolicyContainer> policy_container)
{
    m_policy_container = policy_container;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#descendant-navigables
Vector<GC::Root<HTML::LocalNavigable>> Document::descendant_navigables()
{
    // 1. Let navigables be new list.
    Vector<GC::Root<HTML::LocalNavigable>> navigables;

    // 2. Let navigableContainers be a list of all shadow-including descendants of document that are navigable containers, in shadow-including tree order.
    // 3. For each navigableContainer of navigableContainers:
    for_each_shadow_including_descendant([&](DOM::Node& node) {
        if (is<HTML::NavigableContainer>(node)) {
            auto& navigable_container = static_cast<HTML::NavigableContainer&>(node);
            // 1. If navigableContainer's content navigable is null, then continue.
            if (!navigable_container.content_navigable())
                return TraversalDecision::Continue;

            // 2. Extend navigables with navigableContainer's content navigable's active document's inclusive descendant navigables.
            auto document = as<HTML::LocalNavigable>(*navigable_container.content_navigable()).active_document();
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

Vector<GC::Root<HTML::LocalNavigable>> const Document::descendant_navigables() const
{
    return const_cast<Document&>(*this).descendant_navigables();
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#inclusive-descendant-navigables
Vector<GC::Root<HTML::LocalNavigable>> Document::inclusive_descendant_navigables()
{
    // FIXME: The document's node navigable should not be null here. But we currently do not implement the "unload a
    //        document and its descendants" steps correctly, and the navigable becomes null during unloading. We are
    //        essentially destroying the document too early. See Document::unload_a_document_and_its_descendants. See:
    //        https://github.com/LadybirdBrowser/ladybird/issues/7825
    auto document_node_navigable = navigable();
    if (!document_node_navigable)
        return {};

    // 1. Let navigables be « document's node navigable ».
    Vector<GC::Root<HTML::LocalNavigable>> navigables;
    navigables.append(*document_node_navigable);

    // 2. Extend navigables with document's descendant navigables.
    navigables.extend(descendant_navigables());

    // 3. Return navigables.
    return navigables;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#ancestor-navigables
GC::RootVector<GC::Ref<HTML::Navigable>> Document::ancestor_navigables()
{
    // FIXME: The document's node navigable should not be null here. But we currently do not implement the "unload a
    //        document and its descendants" steps correctly, and the navigable becomes null during unloading. We are
    //        essentially destroying the document too early. See Document::unload_a_document_and_its_descendants. See:
    //        https://github.com/LadybirdBrowser/ladybird/issues/7825
    auto document_node_navigable = navigable();
    if (!document_node_navigable)
        return {};

    // 1. Let navigable be document's node navigable's parent.
    auto navigable = document_node_navigable->parent();

    // 2. Let ancestors be an empty list.
    GC::RootVector<GC::Ref<HTML::Navigable>> ancestors;

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

GC::RootVector<GC::Ref<HTML::Navigable>> const Document::ancestor_navigables() const
{
    return const_cast<Document&>(*this).ancestor_navigables();
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#inclusive-ancestor-navigables
GC::RootVector<GC::Ref<HTML::Navigable>> Document::inclusive_ancestor_navigables()
{
    // FIXME: The document's node navigable should not be null here. But we currently do not implement the "unload a
    //        document and its descendants" steps correctly, and the navigable becomes null during unloading. We are
    //        essentially destroying the document too early. See Document::unload_a_document_and_its_descendants. See:
    //        https://github.com/LadybirdBrowser/ladybird/issues/7825
    auto document_node_navigable = navigable();
    if (!document_node_navigable)
        return {};

    // 1. Let navigables be document's ancestor navigables.
    auto navigables = ancestor_navigables();

    // 2. Append document's node navigable to navigables.
    navigables.append(*document_node_navigable);

    // 3. Return navigables.
    return navigables;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#document-tree-child-navigables
Vector<GC::Root<HTML::LocalNavigable>> Document::document_tree_child_navigables()
{
    // 1. If document's node navigable is null, then return the empty list.
    if (!navigable())
        return {};

    // 2. Let navigables be new list.
    Vector<GC::Root<HTML::LocalNavigable>> navigables;

    // 3. Let navigableContainers be a list of all descendants of document that are navigable containers, in tree order.
    // 4. For each navigableContainer of navigableContainers:
    //     1. If navigableContainer's content navigable is null, then continue.
    //     2. Append navigableContainer's content navigable to navigables.
    // OPTIMIZATION: Iterate all registered navigables to avoid a full tree traversal.
    for (auto const& navigable : HTML::all_local_navigables()) {
        auto container = navigable->container();
        if (!container || !is_ancestor_of(*container))
            continue;
        navigables.insert_before_matching(*navigable, [&](auto const& existing_navigable) {
            return container->is_before(*existing_navigable->container());
        });
    }

    // 5. Return navigables.
    return navigables;
}

// https://html.spec.whatwg.org/multipage/document-lifecycle.html#unloading-document-cleanup-steps
void Document::run_unloading_cleanup_steps()
{
    // 1. Let window be document's relevant global object.
    auto& window = HTML::relevant_window_or_worker_global_scope(*this);

    // 2. For each WebSocket object webSocket whose relevant global object is window, make disappear webSocket.
    //    If this affected any WebSocket objects, then make document unsalvageable given document and "websocket".
    auto affected_any_web_sockets = window.make_disappear_all_web_sockets();
    if (affected_any_web_sockets == HTML::WindowOrWorkerGlobalScopeMixin::AffectedAnyWebSockets::Yes)
        make_unsalvageable("websocket"_utf16);

    // FIXME: 3. For each WebTransport object transport whose relevant global object is window, run the context cleanup steps given transport.

    // 4. If document's salvageable state is false, then:
    if (!m_salvageable) {
        // 1. For each EventSource object eventSource whose relevant global object is equal to window, forcibly close eventSource.
        window.forcibly_close_all_event_sources();

        // 2. Clear window's map of active timers.
        window.clear_map_of_active_timers();
    }

    // https://w3c.github.io/IndexedDB/#database-connection
    // If the execution context where the connection was created is destroyed
    // (for example due to the user navigating away from that page), the connection is closed.
    // AD-HOC: We have no way to detect when the execution context that created the connection is destroyed, and
    //         making LibJS notify us of that would undoubtedly be very costly to performance. All other browsers also
    //         opt not to follow the spec exactly in regards to this, instead letting the connection stay open until
    //         GC collects it. However, we need to be proactive about this when navigating for the sake of test-web.
    window.close_all_idb_connections();
    XHR::XMLHttpRequest::release_activity_roots_for_relevant_global_object(window.this_impl());

    FileAPI::run_unloading_cleanup_steps(*this);
    fully_exit_fullscreen();
}

// https://html.spec.whatwg.org/multipage/document-lifecycle.html#destroy-a-document
void Document::destroy()
{
    // FIXME: 1. Assert: this is running as part of a task queued on document's relevant agent's event loop.

    // 2. Abort document.
    abort();
    stop_compositor_animation_timers();

    // AD-HOC: Notify document observers that this document became inactive.
    //         This handles iframe-removal destruction, which doesn't otherwise go through
    //         did_stop_being_active_document_in_navigable().
    if (m_observers_consider_document_fully_active) {
        m_observers_consider_document_fully_active = false;
        notify_each_document_observer([&](auto const& document_observer) {
            return document_observer.document_became_inactive();
        });
    }

    // 3. Set document's salvageable state to false.
    m_salvageable = false;

    // 4. Let ports be the list of MessagePorts whose relevant global object's associated Document is document.
    // 5. For each port in ports, disentangle port.
    HTML::MessagePort::for_each_message_port([&](HTML::MessagePort& port) {
        auto& global = port.relevant_global_object();
        auto* window = HTML::window_from_global_object(global);
        if (!window)
            return;

        if (&window->associated_document() == this)
            port.disentangle();
    });

    // 6. Run any unloading document cleanup steps for document that are defined by this specification and other applicable specifications.
    run_unloading_cleanup_steps();

    // AD-HOC: Destruction does not go through did_stop_being_active_document_in_navigable(),
    //         but stale per-node and root layout pointers can still keep the old
    //         layout tree alive until GC runs.
    clear_layout_nodes_for_inactive_document();
    tear_down_layout_tree();

    // 7. Remove any tasks whose document is document from any task queue (without running those tasks).
    HTML::main_thread_event_loop().task_queue().remove_tasks_matching([this](auto& task) {
        return task.document() == this;
    });

    // AD-HOC: Mark this document as destroyed so we can remove tasks from the queue that will never be able to run.
    m_has_been_destroyed = true;
    for (auto& observer : m_resize_observers) {
        if (observer)
            observer->document_was_destroyed({});
    }
    m_resize_observers.clear();

    // 8. Set document's browsing context to null.
    m_browsing_context = nullptr;

    // Not in the spec:
    for (auto& navigable_container : HTML::NavigableContainer::all_instances()) {
        if (&navigable_container->document() == this && navigable_container->content_navigable()) {
            auto& child_navigable = as<HTML::LocalNavigable>(*navigable_container->content_navigable());
            child_navigable.set_has_been_destroyed();
            child_navigable.remove_from_all_local_navigables();
        }
    }

    // 9. Set document's node navigable's active session history entry's document state's document to null.
    if (auto navigable = this->navigable()) {
        navigable->set_active_document(nullptr);

        // AD-HOC: We set the page's focused navigable during mouse-down events. If that navigable is this document's
        //         navigable, we must be sure to reset the page's focused navigable.
        page().navigable_document_destroyed({}, *navigable);
    }

    // FIXME: 10. Remove document from the owner set of each WorkerGlobalScope object whose set contains document.
    // FIXME: 11. For each workletGlobalScope in document's worklet global scopes, terminate workletGlobalScope.
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#make-document-unsalvageable
void Document::make_unsalvageable([[maybe_unused]] Utf16View reason)
{
    // FIXME: 1. Let details be a new not restored reason details whose reason is reason.
    // FIXME: 2. Append details to document's bfcache blocking details.

    // 3. Set document's salvageable state to false.
    set_salvageable(false);
}

struct DocumentLifecycleState : public GC::Cell {
    GC_CELL(DocumentLifecycleState, GC::Cell);
    GC_DECLARE_ALLOCATOR(DocumentLifecycleState);

    static constexpr int TIMEOUT_MS = 15000;

    DocumentLifecycleState(GC::Ref<Document> document, size_t remaining, GC::Ref<GC::Function<void()>> finish_callback)
        : remaining_children(remaining)
        , document(document)
        , finish_callback(finish_callback)
        , timeout(Platform::Timer::create_single_shot(GC::Heap::the(), TIMEOUT_MS, GC::create_function(GC::Heap::the(), [this] {
            if (remaining_children > 0)
                dbgln("FIXME: Document unload/destruction timed out with {} remaining children", remaining_children);
        })))
    {
        timeout->start();
    }

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(document);
        visitor.visit(finish_callback);
        visitor.visit(timeout);
    }

    void did_process_child()
    {
        if (--remaining_children > 0)
            return;
        timeout->stop();
        queue_a_task(HTML::Task::Source::NavigationAndTraversal, nullptr, nullptr, finish_callback);
    }

    size_t remaining_children { 0 };
    GC::Ref<Document> document;
    GC::Ref<GC::Function<void()>> finish_callback;
    GC::Ref<Platform::Timer> timeout;
};

GC_DEFINE_ALLOCATOR(DocumentLifecycleState);

// https://html.spec.whatwg.org/multipage/document-lifecycle.html#destroy-a-document-and-its-descendants
void Document::destroy_a_document_and_its_descendants(GC::Ptr<GC::Function<void()>> after_all_destruction)
{
    // 1. If document is not fully active, then:
    if (!is_fully_active()) {
        // 1. Let reason be a string from user-agent specific blocking reasons. If none apply, then let reason be
        //    "masked".
        // FIXME: user-agent specific blocking reasons.
        auto reason = "masked"_utf16;

        // 2. Make document unsalvageable given document and reason.
        make_unsalvageable(reason);

        // FIXME: 3. If document's node navigable is a top-level traversable, build not restored reasons for a top-level
        //    traversable and its descendants given document's node navigable.
    }

    // 2. Let childNavigables be document's child navigables.
    IGNORE_USE_IN_ESCAPING_LAMBDA auto child_navigables = navigable()->child_navigables();

    // 6. Queue a global task on the navigation and traversal task source given document's relevant global object to
    //    perform the following steps:
    auto finish_callback = GC::create_function(GC::Heap::the(), [document = this, after_all_destruction] {
        // 1. Destroy document.
        document->destroy();

        // 2. If afterAllDestruction was given, then run it.
        if (after_all_destruction)
            after_all_destruction->function()();
    });

    // AD-HOC: We avoid allocating a DocumentLifecycleState in case there's no child navigables.
    if (child_navigables.is_empty()) {
        HTML::queue_global_task(HTML::Task::Source::NavigationAndTraversal, relevant_global_object(*this), finish_callback);
        return;
    }

    // 3. Let numberDestroyed be 0.
    auto destruction_state = GC::Heap::the().allocate<DocumentLifecycleState>(*this, child_navigables.size(), finish_callback);

    // 4. For each childNavigable of childNavigables, queue a global task on the navigation and traversal task source
    //    given childNavigable's active window to perform the following steps:
    for (auto& child_navigable : child_navigables) {
        // A child navigable may have already unloaded its active document (and window) by the time
        // the parent starts destruction. In that case there is no global task target; it is already
        // destroyed for the purposes of this algorithm.
        auto active_window = child_navigable->active_window();
        auto increment_destroyed = GC::create_function(GC::Heap::the(), [destruction_state] { destruction_state->did_process_child(); });
        if (!active_window) {
            increment_destroyed->function()();
            continue;
        }

        queue_global_task(HTML::Task::Source::NavigationAndTraversal, HTML::relevant_global_object(*active_window),
            GC::create_function(GC::Heap::the(), [child_navigable, increment_destroyed] {
                // 1. Destroy a document and its descendants given childNavigable's active document and incrementDestroyed.
                if (auto active_document = child_navigable->active_document())
                    active_document->destroy_a_document_and_its_descendants(increment_destroyed);
                else
                    increment_destroyed->function()();
            }));
    }

    // 5. Wait until numberDestroyed equals childNavigable's size.
    // NB: This is handled by destruction_state.
}

// https://html.spec.whatwg.org/multipage/document-lifecycle.html#abort-a-document
void Document::abort()
{
    // 1. Assert: this is running as part of a task queued on document's relevant agent's event loop.

    // 2. Cancel any instances of the fetch algorithm in the context of document,
    //    discarding any tasks queued for them, and discarding any further data received from the network for them.
    //    If this resulted in any instances of the fetch algorithm being canceled
    //    or any queued tasks or any network data getting discarded,
    //    then make document unsalvageable given document and "fetch".
    bool canceled_fetch = false;
    if (m_ongoing_navigation_fetch_controller && m_ongoing_navigation_fetch_controller->state() == Fetch::Infrastructure::FetchController::State::Ongoing) {
        m_ongoing_navigation_fetch_controller->stop_fetch();
        canceled_fetch = true;
    }
    m_ongoing_navigation_fetch_controller = nullptr;

    for (auto& fetch_record : relevant_settings_object().fetch_group()) {
        auto controller = fetch_record.fetch_controller();
        if (!controller || controller->state() != Fetch::Infrastructure::FetchController::State::Ongoing)
            continue;
        controller->stop_fetch();
        canceled_fetch = true;
    }

    if (canceled_fetch)
        make_unsalvageable("fetch"_utf16);

    // 3. If document's during-loading navigation ID for WebDriver BiDi is non-null, then:
    if (m_navigation_id.has_value()) {
        // FIXME: 1. Invoke WebDriver BiDi navigation aborted with document's node navigable and a new WebDriver BiDi
        //           navigation status whose id is document's during-loading navigation ID for WebDriver BiDi, status
        //           is "canceled", and url is document's URL.

        // 2. Set document's during-loading navigation ID for WebDriver BiDi to null.
        m_navigation_id = {};
    }

    // 4. If document has an active parser, then:
    if (auto parser = active_parser()) {
        // 1. Set document's active parser was aborted to true.
        m_active_parser_was_aborted = true;

        // 2. Abort that parser.
        parser->abort();

        // 3. Make document unsalvageable given document and "parser-aborted".
        make_unsalvageable("parser-aborted"_utf16);
    }
}

// https://html.spec.whatwg.org/multipage/document-lifecycle.html#abort-a-document-and-its-descendants
void Document::abort_a_document_and_its_descendants()
{
    // FIXME: 1. Assert: this is running as part of a task queued on document's relevant agent's event loop.

    // 2. Let descendantNavigables be document's descendant navigables.
    auto descendant_navigables = this->descendant_navigables();

    // 3. For each descendantNavigable of descendantNavigables, queue a global task on the navigation and traversal task source given descendantNavigable's active window to perform the following steps:
    for (auto& descendant_navigable : descendant_navigables) {
        HTML::queue_global_task(HTML::Task::Source::NavigationAndTraversal, HTML::relevant_global_object(*descendant_navigable->active_window()), GC::create_function(GC::Heap::the(), [this, descendant_navigable = descendant_navigable.ptr()] {
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

void Document::set_browsing_context(GC::Ptr<HTML::BrowsingContext> browsing_context)
{
    m_browsing_context = browsing_context;
}

// https://html.spec.whatwg.org/multipage/document-lifecycle.html#unload-a-document
void Document::unload(GC::Ptr<Document>)
{
    // FIXME: 1. Assert: this is running as part of a task queued on oldDocument's event loop.

    // FIXME: 2. Let unloadTimingInfo be a new document unload timing info.

    // FIXME: 3. If newDocument is not given, then set unloadTimingInfo to null.

    // FIXME: 4. Otherwise, if newDocument's event loop is not oldDocument's event loop, then the user agent may be unloading
    //    oldDocument in parallel. In that case, the user agent should set unloadTimingInfo to null.

    // 5. Let intendToStoreInBfcache be true if the user agent intends to keep oldDocument alive in a session history
    //    entry, such that it can later be used for history traversal.
    auto intend_to_store_in_bfcache = false;

    // 6. Let eventLoop be oldDocument's relevant agent's event loop.
    auto& event_loop = *HTML::relevant_agent(*this).event_loop;

    // 7. Increase eventLoop's termination nesting level by 1.
    event_loop.increment_termination_nesting_level();

    // 8. Increase oldDocument's unload counter by 1.
    m_unload_counter += 1;

    // 9. If intendToKeepInBfcache is false, then set oldDocument's salvageable state to false.
    if (!intend_to_store_in_bfcache)
        m_salvageable = false;

    // 10. If oldDocument's page showing is true:
    if (m_page_showing) {
        // 1. Set oldDocument's page showing to false.
        m_page_showing = false;

        // 2. Fire a page transition event named pagehide at oldDocument's relevant global object with oldDocument's
        //    salvageable state.
        HTML::relevant_window(*this).fire_a_page_transition_event(HTML::EventNames::pagehide, m_salvageable);
    }

    // 3. Update the visibility state of oldDocument to "hidden".
    // AD-HOC: Violate the spec requirement for the page to be showing (in the spec, this step is a substep of step 10
    //         above — not a sibling step) and instead do this for any page ready for post-load tasks; e.g., the initial
    //         about:blank of a never-navigated child. Gecko/WebKit/Blink consider such pages to be completely loaded —
    //         and update their visibility state when unloading them.
    //         See https://github.com/whatwg/html/issues/12288
    if (m_ready_for_post_load_tasks)
        update_the_visibility_state(HTML::VisibilityState::Hidden);

    // FIXME: 11. If unloadTimingInfo is not null, then set unloadTimingInfo's unload event start time to the current high
    //     resolution time given newDocument's relevant global object, coarsened given oldDocument's relevant settings
    //     object's cross-origin isolated capability.

    // 12. If oldDocument's salvageable state is false, then fire an event named unload at oldDocument's relevant global
    //     object, with legacy target override flag set.
    if (!m_salvageable) {
        // then fire an event named unload at document's relevant global object, with legacy target override flag set.
        // FIXME: The legacy target override flag is currently set by a virtual override of dispatch_event()
        //        We should reorganize this so that the flag appears explicitly here instead.
        auto event = DOM::Event::create(
            HTML::EventNames::unload,
            HighResolutionTime::current_high_resolution_time(relevant_global_object(*this)));
        HTML::relevant_window(*this).dispatch_event(event);
    }

    // FIXME: 13. If unloadTimingInfo is not null, then set unloadTimingInfo's unload event end time to the current high
    //     resolution time given newDocument's relevant global object, coarsened given oldDocument's relevant settings
    //     object's cross-origin isolated capability.

    // 14. Decrease eventLoop's termination nesting level by 1.
    event_loop.decrement_termination_nesting_level();

    // FIXME: 15. Set oldDocument's suspension time to the current high resolution time given document's relevant global object.

    // FIXME: 16. Set oldDocument's suspended timer handles to the result of getting the keys for the map of active timers.

    // FIXME: 17. Set oldDocument's has been scrolled by the user to false.

    // 18. Run any unloading document cleanup steps for oldDocument that are defined by this specification and other
    //     applicable specifications.
    run_unloading_cleanup_steps();

    // 19. If oldDocument's salvageable state is false, then destroy oldDocument.
    if (!m_salvageable)
        destroy();

    // 20. Decrease oldDocument's unload counter by 1.
    m_unload_counter -= 1;

    // FIXME: 21. If newDocument is given, newDocument's was created via cross-origin redirects is false, and newDocument's
    //     origin is the same as oldDocument's origin, then set newDocument's previous document unload timing to
    //     unloadTimingInfo.

    did_stop_being_active_document_in_navigable();
}

// https://html.spec.whatwg.org/multipage/document-lifecycle.html#unload-a-document-and-its-descendants
void Document::unload_a_document_and_its_descendants(GC::Ptr<Document> new_document, GC::Ptr<GC::Function<void()>> after_all_unloads)
{
    // FIXME: 1. Assert: this is running within document's node navigable's traversable navigable's session history traversal
    //    queue.

    // 2. Let childNavigables be document's child navigables.
    IGNORE_USE_IN_ESCAPING_LAMBDA auto child_navigables = navigable()->child_navigables();

    // 6. Queue a global task on the navigation and traversal task source given document's relevant global object to
    //    perform the following steps:
    auto finish_callback = GC::create_function(GC::Heap::the(), [document = this, new_document, after_all_unloads] {
        // FIXME: 1. If firePageSwapSteps is given, then run firePageSwapSteps.

        // 2. Unload document, passing along newDocument if it is not null.
        document->unload(new_document);

        // 3. If afterAllUnloads was given, then run it.
        if (after_all_unloads)
            after_all_unloads->function()();
    });

    // AD-HOC: We avoid allocating a DocumentLifecycleState in case there's no child navigables.
    //         Queue with null document to ensure the task is always runnable. The document
    //         can become non-fully-active during unloading, which would make the task stuck.
    if (child_navigables.is_empty()) {
        HTML::queue_a_task(HTML::Task::Source::NavigationAndTraversal, nullptr, nullptr, finish_callback);
        return;
    }

    // 3. Let numberUnloaded be 0.
    auto unload_state = GC::Heap::the().allocate<DocumentLifecycleState>(*this, child_navigables.size(), finish_callback);

    // 4. For each childNavigable of childNavigables [[ in what order? ]], queue a global task on the navigation and
    //    traversal task source given childNavigable's active window to perform the following steps:
    for (auto& child_navigable : child_navigables) {
        HTML::queue_a_task(HTML::Task::Source::NavigationAndTraversal, nullptr, nullptr,
            GC::create_function(GC::Heap::the(), [unload_state, child_navigable = child_navigable.ptr()] {
                // 1. Let incrementUnloaded be an algorithm step which increments numberUnloaded.
                auto increment_unloaded = GC::create_function(GC::Heap::the(), [unload_state] { unload_state->did_process_child(); });

                // 2. Unload a document and its descendants given childNavigable's active document, null, and incrementUnloaded.
                if (auto active_document = child_navigable->active_document())
                    active_document->unload_a_document_and_its_descendants({}, increment_unloaded);
                else
                    increment_unloaded->function()();
            }));
    }

    // 5. Wait until numberUnloaded equals childNavigables's size.
    // NB: This is handled by unload_state.
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
        // FIXME: Implement allowlist for this.
        return true;
    case PolicyControlledFeature::Camera:
        // FIXME: Implement allowlist for this.
        return true;
    case PolicyControlledFeature::FocusWithoutUserActivation:
    case PolicyControlledFeature::EncryptedMedia:
        // FIXME: Implement allowlist for this.
        return true;
    case PolicyControlledFeature::Fullscreen:
        // FIXME: Implement allowlist for this.
        return true;
    case PolicyControlledFeature::Gamepad:
        // FIXME: Implement allowlist for this.
        return true;
    case PolicyControlledFeature::Microphone:
        // FIXME: Implement allowlist for this.
        return true;
    case PolicyControlledFeature::WindowManagement:
        // FIXME: Implement allowlist for this.
        return true;
    }

    // 4. Return false.
    return false;
}

void Document::did_stop_being_active_document_in_navigable()
{
    stop_compositor_animation_timers();
    clear_layout_nodes_for_inactive_document();
    tear_down_layout_tree();

    schedule_html_parser_end_check();

    if (m_observers_consider_document_fully_active) {
        m_observers_consider_document_fully_active = false;
        notify_each_document_observer([&](auto const& document_observer) {
            return document_observer.document_became_inactive();
        });
    }
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
    // 1. If document is not a Document created by this algorithm:
    if (!created_for_appropriate_template_contents()) {
        // 1. If document does not yet have an associated inert template document:
        if (!m_associated_inert_template_document) {
            // 1. Let newDocument be a new Document (whose browsing context is null). This is "a Document created by
            //    this algorithm" for the purposes of the step above.
            auto new_document = HTML::HTMLDocument::create(page(), *m_relevant_global_event_target);
            new_document->m_created_for_appropriate_template_contents = true;

            // 2. If document is an HTML document, then mark newDocument as an HTML document also.
            if (document_type() == Type::HTML)
                new_document->set_document_type(Type::HTML);

            // 3. Set document's associated inert template document to newDocument.
            m_associated_inert_template_document = new_document;
        }
        // 2. Set document to document's associated inert template document.
        return *m_associated_inert_template_document;
    }
    // 2. Return document.
    return *this;
}

Utf16String Document::dump_accessibility_tree_as_json()
{
    update_layout(UpdateLayoutReason::InspectAccessibilityTree);

    Utf16StringBuilder builder;
    auto accessibility_tree = AccessibilityTreeNode::create(nullptr);
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
    return builder.to_string();
}

// https://dom.spec.whatwg.org/#dom-document-createattribute
WebIDL::ExceptionOr<GC::Ref<Attr>> Document::create_attribute(Utf16FlyString const& local_name)
{
    // 1. If localName is not a valid attribute local name, then throw an "InvalidCharacterError" DOMException.
    if (!is_valid_attribute_local_name(local_name))
        return WebIDL::InvalidCharacterError::create("Invalid character in attribute name."_utf16);

    // 2. If this is an HTML document, then set localName to localName in ASCII lowercase.
    // 3. Return a new attribute whose local name is localName and node document is this.
    if (is_html_document())
        return Attr::create(*this, local_name.to_ascii_lowercase());
    return Attr::create(*this, local_name);
}

// https://dom.spec.whatwg.org/#dom-document-createattributens
WebIDL::ExceptionOr<GC::Ref<Attr>> Document::create_attribute_ns(Optional<Utf16FlyString> namespace_, Utf16FlyString const& qualified_name)
{
    // 1. Let (namespace, prefix, localName) be the result of validating and extracting namespace and qualifiedName given "attribute".
    auto fly_namespace = namespace_.map([](auto const& value) {
        return FlyString { value.view().to_utf8_but_should_be_ported_to_utf16() };
    });
    auto fly_qualified_name = FlyString { qualified_name.view().to_utf8_but_should_be_ported_to_utf16() };
    auto extracted_qualified_name_or_error = validate_and_extract(move(fly_namespace), fly_qualified_name, ValidationContext::Attribute);
    if (extracted_qualified_name_or_error.is_error())
        return validate_and_extract_error_to_dom_exception(extracted_qualified_name_or_error.release_error());
    auto extracted_qualified_name = extracted_qualified_name_or_error.release_value();

    // 2. Return a new attribute whose namespace is namespace, namespace prefix is prefix, local name is localName, and node document is this.
    return Attr::create(*this, extracted_qualified_name);
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#make-active
void Document::make_active()
{
    // 1. Let window be document's relevant global object.
    auto& window = HTML::relevant_window(*this);

    set_window(window);

    // 2. Set document's browsing context's active document to document.
    m_browsing_context->set_active_document(*this);

    // 3. Set document's browsing context's WindowProxy's [[Window]] internal slot value to window.
    m_browsing_context->set_active_window(window);

    auto current_navigable = this->navigable();
    if (current_navigable && current_navigable->is_top_level_traversable()) {
        page().client().page_did_change_active_document_in_top_level_browsing_context(*this);
    }

    // 4. Set window's relevant settings object's execution ready flag.
    HTML::relevant_settings_object(window).execution_ready = true;

    if (m_needs_to_call_page_did_load) {
        navigable()->traversable_navigable()->page().client().page_did_finish_loading(m_navigation_id, url());
        m_needs_to_call_page_did_load = false;
    }

    if (!m_observers_consider_document_fully_active) {
        m_observers_consider_document_fully_active = true;
        notify_each_document_observer([&](auto const& document_observer) {
            return document_observer.document_became_active();
        });
    }
}

// https://html.spec.whatwg.org/multipage/interaction.html#set-the-initial-visibility-state
void Document::set_initial_visibility_state(HTML::VisibilityState visibility_state)
{
    // 1. Set document's visibility state to visibility state.
    m_visibility_state = visibility_state;

    // TODO: 2. Queue a new VisibilityStateEntry whose visibility state is document's visibility state and whose timestamp is 0.

    // AD-HOC: Record the initial viewport and visual viewport state so that if the viewport changes before the
    //         first rendering update (e.g. in our fullscreen tests), change events are still fired.
    if (!m_last_viewport_size.has_value()) {
        m_last_viewport_size = viewport_rect().size().to_type<int>();
        auto& current_visual_viewport = *visual_viewport();
        m_last_visual_viewport_state = VisualViewportState { current_visual_viewport.scale(), { current_visual_viewport.width(), current_visual_viewport.height() } };
    }
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
    VERIFY(!m_intersection_observers.contains(observer));
    m_intersection_observers.set(observer);
}

void Document::unregister_intersection_observer(Badge<IntersectionObserver::IntersectionObserver>, IntersectionObserver::IntersectionObserver& observer)
{
    bool was_removed = m_intersection_observers.remove(observer);
    VERIFY(was_removed);
}

void Document::register_resize_observer(Badge<ResizeObserver::ResizeObserver>, ResizeObserver::ResizeObserver& observer)
{
    if (!m_resize_observers.contains_slow(GC::Weak<ResizeObserver::ResizeObserver> { observer }))
        m_resize_observers.append(observer);
}

void Document::unregister_resize_observer(Badge<ResizeObserver::ResizeObserver>, ResizeObserver::ResizeObserver& observer)
{
    m_resize_observers.remove_all_matching([&](auto const& entry) { return !entry || entry.ptr().ptr() == &observer; });
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
    HTML::queue_global_task(HTML::Task::Source::IntersectionObserver, HTML::relevant_global_object(*window), GC::create_function(GC::Heap::the(), [this]() {
        // https://www.w3.org/TR/intersection-observer/#notify-intersection-observers
        // 1. Set document’s IntersectionObserverTaskQueued flag to false.
        m_intersection_observer_task_queued = false;

        // 2. Let notify list be a list of all IntersectionObservers whose root is in the DOM tree of document.
        GC::RootVector<GC::Ref<IntersectionObserver::IntersectionObserver>> notify_list;
        for (auto& observer : m_intersection_observers)
            notify_list.append(observer);

        // 3. For each IntersectionObserver object observer in notify list, run these steps:
        for (auto& observer : notify_list) {
            // 2. Let queue be a copy of observer’s internal [[QueuedEntries]] slot.
            // 3. Clear observer’s internal [[QueuedEntries]] slot.
            auto queue = observer->take_records();

            // 1. If observer’s internal [[QueuedEntries]] slot is empty, continue.
            if (queue.is_empty())
                continue;

            // 4. Let callback be the value of observer’s internal [[callback]] slot.

            // 5. Invoke callback with queue as the first argument, observer as the second argument, and observer as the callback this value. If this throws an exception, report the exception.
            IntersectionObserver::invoke_intersection_observer_callback(*observer, queue);
        }
    }));
}

// https://www.w3.org/TR/intersection-observer/#queue-an-intersectionobserverentry
void Document::queue_an_intersection_observer_entry(IntersectionObserver::IntersectionObserver& observer, HighResolutionTime::DOMHighResTimeStamp time, GC::Ref<Geometry::DOMRectReadOnly> root_bounds, GC::Ref<Geometry::DOMRectReadOnly> bounding_client_rect, GC::Ref<Geometry::DOMRectReadOnly> intersection_rect, bool is_intersecting, double intersection_ratio, GC::Ref<Element> target)
{
    // 1. Construct an IntersectionObserverEntry, passing in time, rootBounds, boundingClientRect, intersectionRect, isIntersecting, and target.
    auto entry = IntersectionObserver::IntersectionObserverEntry::create(time, root_bounds, bounding_client_rect, intersection_rect, is_intersecting, intersection_ratio, target);

    // 2. Append it to observer’s internal [[QueuedEntries]] slot.
    observer.queue_entry({}, entry);

    // 3. Queue an intersection observer task for document.
    queue_intersection_observer_task();
}

// https://www.w3.org/TR/intersection-observer/#compute-the-intersection
static CSSPixelRect compute_intersection(GC::Ref<Element> target, CSSPixelRect target_rect, IntersectionObserver::IntersectionObserver const& observer, Layout::Box const* root_layout_box, CSSPixelRect const& root_bounds, Painting::AccumulatedVisualContextTree const& visual_context_tree)
{
    // 1. Let intersectionRect be the result of getting the bounding box for target.
    auto intersection_rect = target_rect;

    // 2. Let container be the containing block of target.
    // 3. While container is not root:
    auto const* target_layout_node = target->layout_node();
    if (target_layout_node && Painting::has_committed_box(*target_layout_node)) {
        for (auto const* container_box = target_layout_node->containing_block(); container_box; container_box = container_box->containing_block()) {
            // Stop when we reach the intersection root.
            if (container_box == root_layout_box)
                break;
            if (!Painting::has_committed_box(*container_box))
                break;

            // FIXME: 3.1. If container is the document of a nested browsing context, update
            //             intersectionRect by clipping to the viewport of the document, and update
            //             container to be the browsing context container of container.

            // NOTE: Steps 3.2 (map to container coordinate space) and 3.5 (update container) are
            //       unnecessary here because get_bounding_client_rect() and transform_rect_to_viewport()
            //       already produce viewport-relative coordinates.

            // 3.3. If container is a scroll container, apply the observer’s [[scrollMargin]]
            //      to the container’s clip rect.
            // 3.4. If container has a content clip or a css clip-path property, update intersectionRect
            //      by applying container’s clip.
            // FIXME: Handle clip-path.
            auto overflow_x = container_box->overflow_x();
            auto overflow_y = container_box->overflow_y();
            bool has_content_clip = overflow_x != CSS::Overflow::Visible || overflow_y != CSS::Overflow::Visible;
            if (has_content_clip) {
                auto clip_rect = Painting::transform_rect_to_viewport(*container_box, Painting::absolute_padding_box_rect(*container_box), visual_context_tree, Painting::AccumulatedVisualContextTree::IncludeVisualViewportTransform::No);

                // Apply scroll margin to expand the scrollport for scroll containers.
                auto& scroll_margin = observer.scroll_margin_values();
                if (container_box->is_scroll_container() && !scroll_margin.is_empty()) {
                    clip_rect.inflate(
                        scroll_margin[0].to_px(clip_rect.height()),
                        scroll_margin[1].to_px(clip_rect.width()),
                        scroll_margin[2].to_px(clip_rect.height()),
                        scroll_margin[3].to_px(clip_rect.width()));
                }

                intersection_rect.intersect(clip_rect);
            }
        }
    }

    // FIXME: 4. Map intersectionRect to the coordinate space of root.

    // 5. Update intersectionRect by intersecting it with the root intersection rectangle.
    intersection_rect.intersect(root_bounds);

    // FIXME: 6. Map intersectionRect to the coordinate space of the viewport of the document containing target.

    // 7. Return intersectionRect.
    return intersection_rect;
}

// https://www.w3.org/TR/intersection-observer/#run-the-update-intersection-observations-steps
void Document::run_the_update_intersection_observations_steps(HighResolutionTime::DOMHighResTimeStamp time)
{
    // 1. Let observer list be a list of all IntersectionObservers whose root is in the DOM tree of document.
    //    For the top-level browsing context, this includes implicit root observers.
    // 2. For each observer in observer list:

    // NOTE: We make a copy of the intersection observers list to avoid modifying it while iterating.
    GC::RootVector<GC::Ref<IntersectionObserver::IntersectionObserver>> intersection_observers;
    for (auto& observer : m_intersection_observers)
        intersection_observers.append(observer);

    update_paint_and_hit_testing_properties_if_needed();

    HashMap<Document*, Painting::AccumulatedVisualContextTree> observation_visual_context_trees;
    auto sample_time_ns = MonotonicTime::now().nanoseconds();
    auto sampled_visual_context_tree = [&](Document& document) -> Optional<Painting::AccumulatedVisualContextTree> {
        if (!document.m_paint_state)
            return {};
        if (auto cached_tree = observation_visual_context_trees.get(&document); cached_tree.has_value())
            return *cached_tree;
        auto committed_tree = document.paint_state().visual_context_tree(document);
        auto sampled_tree = committed_tree.visual_animations().is_empty()
            ? committed_tree
            : committed_tree.with_visual_animation_samples(sample_time_ns);
        observation_visual_context_trees.set(&document, sampled_tree);
        return sampled_tree;
    };

    for (auto& observer : intersection_observers) {
        // 1. Let rootBounds be observer’s root intersection rectangle.
        auto root_visual_context_tree = sampled_visual_context_tree(observer->intersection_root_node()->document());
        if (!root_visual_context_tree.has_value())
            continue;
        auto root_bounds = observer->root_intersection_rectangle(&*root_visual_context_tree);

        // Pre-compute per-observer values to avoid repeated work in the per-target loop.
        auto intersection_root_node = observer->intersection_root_node();
        Layout::Box const* root_layout_box = nullptr;
        if (auto const* root_layout_node = intersection_root_node->layout_node(); root_layout_node && Painting::has_committed_box(*root_layout_node))
            root_layout_box = as<Layout::Box>(root_layout_node);
        bool is_implicit_root = observer->is_implicit_root();
        bool root_is_element = intersection_root_node->is_element();

        // 2. For each target in observer’s internal [[ObservationTargets]] slot, processed in the same order that
        //    observe() was called on each target:
        for (auto& observed_target : observer->observation_targets()) {
            auto& target = observed_target.target;
            // 1. Let:
            // thresholdIndex be 0.
            size_t threshold_index = 0;

            // isIntersecting be false.
            bool is_intersecting = false;

            bool intersection_geometry_intersects = false;

            // targetRect be a DOMRectReadOnly with x, y, width, and height set to 0.
            CSSPixelRect target_rect { 0, 0, 0, 0 };

            // intersectionRect be a DOMRectReadOnly with x, y, width, and height set to 0.
            CSSPixelRect intersection_rect { 0, 0, 0, 0 };

            // SPEC ISSUE: It doesn’t pass in intersection ratio to "queue an IntersectionObserverEntry" despite needing it.
            //             This is default 0, as isIntersecting is default false, see step 9.
            double intersection_ratio = 0.0;

            // 2. If the intersection root is not the implicit root, and target is not in the same document as the intersection root, skip to step 11.
            // 3. If the intersection root is an Element, and target is not a descendant of the intersection root in the containing block chain, skip to step 11.
            // FIXME: Actually use the containing block chain.
            // NOTE: Check if target has a layout node is not in the spec but required to match other browsers.
            // AD-HOC: A target whose document was excluded from this rendering update has stale layout; treat it as
            //         not intersecting like other engines instead of reading its geometry.
            if (target->document().layout_is_up_to_date() && target->layout_node() && (is_implicit_root || &target->document() == &intersection_root_node->document()) && !(root_is_element && !target->is_descendant_of(*intersection_root_node))) {
                auto target_visual_context_tree = sampled_visual_context_tree(target->document());
                if (!target_visual_context_tree.has_value())
                    continue;

                // 4. Set targetRect to the DOMRectReadOnly obtained by getting the bounding box for target.
                target_rect = target->bounding_client_rect_assuming_layout_clean(*target_visual_context_tree);

                // 5. Let intersectionRect be the result of running the compute the intersection algorithm on target and
                //    observer’s intersection root.
                intersection_rect = compute_intersection(target, target_rect, *observer, root_layout_box, root_bounds, *target_visual_context_tree);

                // 6. Let targetArea be targetRect’s area.
                auto target_area = target_rect.width() * target_rect.height();

                // 7. Let intersectionArea be intersectionRect’s area.
                auto intersection_area = intersection_rect.size().area();

                // 8. Let isIntersecting be true if targetRect and rootBounds intersect or are edge-adjacent, even if the
                //    intersection has zero area (because rootBounds or targetRect have zero area).
                intersection_geometry_intersects = target_rect.edge_adjacent_intersects(root_bounds);

                // 9. If targetArea is non-zero, let intersectionRatio be intersectionArea divided by targetArea.
                //    Otherwise, let intersectionRatio be 1 if isIntersecting is true, or 0 if isIntersecting is false.
                if (target_area != 0.0)
                    intersection_ratio = (intersection_area / target_area).to_double();
                else
                    intersection_ratio = intersection_geometry_intersects ? 1.0 : 0.0;

                // 10. Set thresholdIndex to the index of the first entry in observer.thresholds whose value is greater
                //     than intersectionRatio, or the length of observer.thresholds if intersectionRatio is greater than
                //     or equal to the last entry in observer.thresholds.
                // NB: Thresholds are sorted in ascending order, so we use binary search.
                if (intersection_geometry_intersects) {
                    auto const& thresholds = observer->thresholds();
                    size_t lo = 0;
                    size_t hi = thresholds.size();
                    while (lo < hi) {
                        size_t mid = lo + (hi - lo) / 2;
                        if (thresholds[mid] > intersection_ratio)
                            hi = mid;
                        else
                            lo = mid + 1;
                    }
                    threshold_index = lo;
                }

                // INTEROP: All other engines report isIntersecting as false when the intersection ratio is below the
                //          first threshold. See https://github.com/w3c/IntersectionObserver/issues/432.
                is_intersecting = threshold_index > 0;
            }

            // 11. Let intersectionObserverRegistration be the IntersectionObserverRegistration record in target’s
            //     internal [[RegisteredIntersectionObservers]] slot whose observer property is equal to observer.
            // NB: This implementation deviates from the spec's storage model. intersectionObserverRegistration here
            //     aliases the observer-side observation target state, since target's registered observers only keep
            //     the observer reference. This avoids an extra lookup through target on every update.
            auto& intersection_observer_registration = observed_target;

            // 12. Let previousThresholdIndex be the intersectionObserverRegistration’s previousThresholdIndex property.
            auto previous_threshold_index = intersection_observer_registration.previous_threshold_index;

            // 13. Let previousIsIntersecting be the intersectionObserverRegistration’s previousIsIntersecting property.
            auto previous_is_intersecting = intersection_observer_registration.previous_is_intersecting;

            // 14. If thresholdIndex does not equal previousThresholdIndex or if isIntersecting does not equal
            //     previousIsIntersecting, queue an IntersectionObserverEntry, passing in observer, time,
            //     rootBounds, targetRect, intersectionRect, isIntersecting, and target.
            if (threshold_index != previous_threshold_index || is_intersecting != previous_is_intersecting) {
                auto root_bounds_as_dom_rect = Geometry::DOMRectReadOnly::create(static_cast<double>(root_bounds.x()), static_cast<double>(root_bounds.y()), static_cast<double>(root_bounds.width()), static_cast<double>(root_bounds.height()));

                // SPEC ISSUE: It doesn't pass in intersectionRatio, but it's required.
                auto target_dom_rect = Geometry::DOMRectReadOnly::create(static_cast<double>(target_rect.x()), static_cast<double>(target_rect.y()), static_cast<double>(target_rect.width()), static_cast<double>(target_rect.height()));
                auto intersection_dom_rect = Geometry::DOMRectReadOnly::create(static_cast<double>(intersection_rect.x()), static_cast<double>(intersection_rect.y()), static_cast<double>(intersection_rect.width()), static_cast<double>(intersection_rect.height()));
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

    // 1. Let doc be element's node document.
    VERIFY(&element.document() == this);

    // 2. If doc's lazy load intersection observer is null, set it to a new IntersectionObserver instance, initialized as follows:
    if (!m_lazy_load_intersection_observer) {
        auto options = IntersectionObserver::IntersectionObserverOptions {};
        // The HTML lazy-loading algorithm uses a generous implicit root margin so that resources are fetched
        // before they enter the viewport.  In particular, this also handles elements whose box is initially empty
        // while their intrinsic dimensions are waiting on the fetch itself.
        options.root_margin = "1250px"_utf16;

        // The callback is these steps, with arguments entries and observer:
        auto wrapped_callback = IntersectionObserver::create_lazy_load_intersection_observer_callback(*this);
        m_lazy_load_intersection_observer = IntersectionObserver::IntersectionObserver::create_with_implicit_root_document(wrapped_callback, options, *this).release_value_but_fixme_should_propagate_errors();
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

void Document::process_lazy_load_intersection_observer_entries(ReadonlySpan<GC::Ref<IntersectionObserver::IntersectionObserverEntry>> entries)
{
    // For each entry in entries using a method of iteration which does not
    // trigger developer-modifiable array accessors or iteration hooks:
    for (auto entry : entries) {
        // 1. Let resumptionSteps be null.
        GC::Ptr<GC::Function<void()>> resumption_steps;

        // 2. If entry.isIntersecting is true, then set resumptionSteps to
        // entry.target's lazy load resumption steps.
        if (entry->is_intersecting()) {
            // 5. Set entry.target's lazy load resumption steps to null.
            VERIFY(entry->target()->is_lazy_loading());
            resumption_steps = entry->target()->take_lazy_load_resumption_steps({});
        }

        // 3. If resumptionSteps is null, then continue.
        if (!resumption_steps)
            continue;

        // 4. Stop intersection-observing a lazy loading element for entry.target.
        stop_intersection_observing_a_lazy_loading_element(entry->target());

        // 5. Set entry.target's lazy load resumption steps to null.
        entry->target()->take_lazy_load_resumption_steps({});

        // 6. Invoke resumptionSteps.
        resumption_steps->function()();
    }
}

// https://html.spec.whatwg.org/multipage/semantics.html#shared-declarative-refresh-steps
void Document::shared_declarative_refresh_steps(Utf16View input, GC::Ptr<HTML::HTMLMetaElement const> meta_element)
{
    // 1. If document's will declaratively refresh is true, then return.
    if (m_will_declaratively_refresh)
        return;

    // 2. Let position point at the first code point of input.
    Utf16GenericLexer lexer(input);

    // 3. Skip ASCII whitespace within input given position.
    lexer.ignore_while(Infra::is_ascii_whitespace);

    // 4. Let time be 0.
    u32 time = 0;

    // 5. Collect a sequence of code points that are ASCII digits from input given position, and let timeString be the result.
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
        Optional<u16> quote;
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
        // 11. Parse: Set urlRecord to the result of encoding-parsing a URL given urlString, relative to document.
        // 12. If urlRecord is failure, then return.
        auto maybe_url_record = encoding_parse_url(url_string);
        if (!maybe_url_record.has_value())
            return;

        url_record = maybe_url_record.release_value();

        // 13. If urlRecord's scheme is "javascript", then return.
        if (url_record.scheme() == "javascript"sv)
            return;
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

        MUST(navigable->navigate({ .url = url_record, .source_document = *this, .history_handling = HTML::NavigationHistoryBehavior::Replace }));
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

Painting::DocumentPaintState& Document::paint_state()
{
    VERIFY(m_paint_state);
    return *m_paint_state;
}

Painting::DocumentPaintState const& Document::paint_state() const
{
    VERIFY(m_paint_state);
    return *m_paint_state;
}

Painting::AccumulatedVisualContextTree Document::visual_context_tree() const
{
    return paint_state().visual_context_tree(*this);
}

u64 Document::visual_context_tree_structural_epoch() const
{
    return paint_state().visual_context_tree_structural_epoch(*this);
}

Painting::ScrollStateSnapshot const& Document::scroll_state_snapshot() const
{
    return paint_state().scroll_state_snapshot();
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#restore-the-history-object-state
void Document::restore_the_history_object_state(NonnullRefPtr<HTML::SessionHistoryEntry> entry)
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
void Document::update_for_history_step_application(NonnullRefPtr<HTML::SessionHistoryEntry> entry, bool do_not_reactivate, size_t script_history_length, size_t script_history_index, Optional<HTML::NavigationType> navigation_type, Optional<Vector<NonnullRefPtr<HTML::SessionHistoryEntry>>> entries_for_navigation_api, RefPtr<HTML::SessionHistoryEntry> previous_entry_for_activation, bool update_navigation_api)
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
    auto navigation = HTML::relevant_window(*this).navigation();

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

            // AD HOC: Skip this in situations the spec steps don't account for
            if (update_navigation_api) {
                // 1. Assert: navigationType is not null.
                VERIFY(navigation_type.has_value());
                // 2. Update the navigation API entries for a same-document navigation given navigation, entry, and navigationType.
                navigation->update_the_navigation_api_entries_for_a_same_document_navigation(entry, navigation_type.value());
            }

            // 3. Fire an event named popstate at document's relevant global object, using PopStateEvent,
            //    with the state attribute initialized to document's history object's state and hasUAVisualTransition initialized to true
            //    if a visual transition, to display a cached rendered state of the latest entry, was done by the user agent.
            // FIXME: Initialise hasUAVisualTransition
            auto& relevant_global_object = HTML::relevant_global_object(*this);
            auto& window = HTML::relevant_window(*this);
            auto pop_state_event = HTML::PopStateEvent::create(
                "popstate"_fly_string,
                history()->state_value(),
                HighResolutionTime::current_high_resolution_time(relevant_global_object));
            window.dispatch_event(pop_state_event);

            // 4. Restore persisted state given entry.
            if (auto navigable = this->navigable())
                navigable->restore_persisted_state_from_session_history_entry(*entry);

            // 5. If oldURL's fragment is not equal to entry's URL's fragment, then queue a global task on the DOM manipulation task source
            //    given document's relevant global object to fire an event named hashchange at document's relevant global object,
            //    using HashChangeEvent, with the oldURL attribute initialized to the serialization of oldURL and the newURL attribute
            //    initialized to the serialization of entry's URL.
            if (old_url.fragment() != entry->url().fragment()) {
                auto hashchange_event = HTML::HashChangeEvent::create(
                    "hashchange"_fly_string,
                    old_url.serialize(),
                    entry->url().serialize(),
                    HighResolutionTime::current_high_resolution_time(relevant_global_object));
                HTML::queue_global_task(HTML::Task::Source::DOMManipulation, relevant_global_object, GC::create_function(GC::Heap::the(), [hashchange_event, &window]() {
                    window.dispatch_event(hashchange_event);
                }));
            }
        }

        // 5. Otherwise:
        else {
            // 1. Assert: entriesForNavigationAPI is given.
            VERIFY(!update_navigation_api || entries_for_navigation_api.has_value());

            // 2. Restore persisted state given entry.
            if (auto navigable = this->navigable()) {
                navigable->restore_persisted_state_from_session_history_entry(*entry);
                navigable->schedule_persisted_state_restoration_retry(*entry);
            }

            // 3. Initialize the navigation API entries for a new document given navigation, entriesForNavigationAPI, and entry.
            if (update_navigation_api)
                navigation->initialize_the_navigation_api_entries_for_a_new_document(
                    *entries_for_navigation_api, entry);
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
        set_ready_to_run_scripts();
    }

    // 9. Otherwise, if documentsEntryChanged is false and doNotReactivate is false, then:
    // NOTE: This is for bfcache restoration
    if (!documents_entry_changed && !do_not_reactivate) {
        // FIXME: 1. Assert: entriesForNavigationAPI is given.
        // FIXME: 2. Reactivate document given entry and entriesForNavigationAPI.
    }
}

void Document::set_ready_to_run_scripts()
{
    m_ready_to_run_scripts = true;
    if (auto callback = m_deferred_parser_start) {
        m_deferred_parser_start = nullptr;
        callback->function()();
    }
}

void Document::set_deferred_parser_start(GC::Ref<GC::Function<void()>> callback)
{
    VERIFY(!m_deferred_parser_start);
    m_deferred_parser_start = callback;
}

void Document::set_latest_entry(RefPtr<HTML::SessionHistoryEntry> entry)
{
    m_latest_entry = move(entry);
}

HashMap<URL::URL, GC::Ptr<HTML::SharedResourceRequest>>& Document::shared_resource_requests()
{
    return m_shared_resource_requests;
}

HashMap<URL::URL, GC::Ptr<HTML::SharedResourceRequest>> const& Document::shared_resource_requests() const
{
    return m_shared_resource_requests;
}

CSS::ImageStyleValueResource* Document::css_image_resource(URL::URL const& url)
{
    auto it = m_css_image_resources.find(url);
    if (it == m_css_image_resources.end())
        return nullptr;
    return it->value.ptr();
}

CSS::ImageStyleValueResource& Document::create_css_image_resource(GC::Ref<HTML::SharedResourceRequest> request)
{
    // NB: The caller should guard against creating already existing resources.
    VERIFY(!m_css_image_resources.contains(request->url()));

    auto resource = make<CSS::ImageStyleValueResource>(request, *this);
    auto& resource_ref = *resource;
    m_css_image_resources.set(request->url(), move(resource));
    return resource_ref;
}

void Document::remove_css_image_resource_if_unused(URL::URL const& url)
{
    auto it = m_css_image_resources.find(url);
    if (it == m_css_image_resources.end())
        return;
    if (!it->value->can_be_removed())
        return;
    m_css_image_resources.remove(it);
}

void Document::prune_image_resource_caches()
{
    static constexpr size_t decoded_image_resource_cache_limit = 8 * MiB;
    static constexpr size_t decoded_image_resource_cache_count_limit = 96;

    auto is_used_by_css_image_resource = [&](URL::URL const& url, HTML::SharedResourceRequest const& request) {
        auto* css_image_resource = this->css_image_resource(url);
        return css_image_resource && css_image_resource->decoded_image_data() == request.image_data();
    };

    struct CacheSize {
        size_t decoded_image_size { 0 };
        size_t decoded_image_count { 0 };
    };

    auto cache_size = [&] {
        CacheSize cache_size;
        size_t size = 0;
        size_t count = 0;
        for (auto const& it : m_shared_resource_requests) {
            auto const& request = *it.value;
            if (!request.can_be_pruned_from_memory_cache())
                continue;
            if (is_used_by_css_image_resource(it.key, request))
                continue;
            ++count;
            if (auto image_data = request.image_data())
                size += image_data->external_memory_size();
        }
        cache_size.decoded_image_size = size;
        cache_size.decoded_image_count = count;
        return cache_size;
    };

    auto size = cache_size();
    while (size.decoded_image_size > decoded_image_resource_cache_limit || size.decoded_image_count > decoded_image_resource_cache_count_limit) {
        Optional<URL::URL> least_recently_used_url;
        u64 least_recently_used_serial = NumericLimits<u64>::max();

        for (auto const& it : m_shared_resource_requests) {
            auto const& request = *it.value;
            if (!request.can_be_pruned_from_memory_cache())
                continue;
            if (is_used_by_css_image_resource(it.key, request))
                continue;
            if (request.cache_touch_serial() >= least_recently_used_serial)
                continue;
            least_recently_used_url = it.key;
            least_recently_used_serial = request.cache_touch_serial();
        }

        if (!least_recently_used_url.has_value())
            break;

        m_shared_resource_requests.remove(least_recently_used_url.value());

        auto new_size = cache_size();
        if (new_size.decoded_image_size == size.decoded_image_size && new_size.decoded_image_count == size.decoded_image_count)
            break;
        size = new_size;
    }

    m_list_of_available_images->prune_to_limits(decoded_image_resource_cache_limit, decoded_image_resource_cache_count_limit);
}

// https://www.w3.org/TR/web-animations-1/#dom-document-timeline
GC::Ref<Animations::DocumentTimeline> Document::timeline()
{
    // The DocumentTimeline object representing the default document timeline. The default document timeline has an
    // origin time of zero.
    if (!m_default_timeline)
        m_default_timeline = Animations::DocumentTimeline::create(*this, 0.0);
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

void Document::associate_with_animation(GC::Ref<Animations::Animation> animation)
{
    m_associated_animations.set(animation);
    if (m_style_stabilization_epoch_depth > 0)
        m_animations_created_in_stabilization_epoch.set(animation);
}

void Document::disassociate_with_animation(GC::Ref<Animations::Animation> animation)
{
    if (animation->effect() && is<Animations::KeyframeEffect>(*animation->effect())) {
        auto& effect = static_cast<Animations::KeyframeEffect&>(*animation->effect());
        m_effects_needing_animated_style_update.remove(effect);
        m_effects_needing_animated_style_update_after_current_update.remove(effect);
    }
    m_associated_animations.remove(animation);
}

size_t Document::associated_animation_count() const
{
    size_t count = 0;
    for ([[maybe_unused]] auto& animation : m_associated_animations)
        ++count;
    return count;
}

static Optional<Compositor::VisualAnimationTransformOperationKind> compositor_transform_operation_kind(CSS::TransformFunction function)
{
    using CSS::TransformFunction;
    using Kind = Compositor::VisualAnimationTransformOperationKind;
    switch (function) {
    case TransformFunction::Translate:
        return Kind::Translate;
    case TransformFunction::Translate3d:
        return Kind::Translate3d;
    case TransformFunction::TranslateX:
        return Kind::TranslateX;
    case TransformFunction::TranslateY:
        return Kind::TranslateY;
    case TransformFunction::TranslateZ:
        return Kind::TranslateZ;
    case TransformFunction::Scale:
        return Kind::Scale;
    case TransformFunction::Scale3d:
        return Kind::Scale3d;
    case TransformFunction::ScaleX:
        return Kind::ScaleX;
    case TransformFunction::ScaleY:
        return Kind::ScaleY;
    case TransformFunction::ScaleZ:
        return Kind::ScaleZ;
    case TransformFunction::Rotate:
        return Kind::Rotate;
    case TransformFunction::RotateX:
        return Kind::RotateX;
    case TransformFunction::RotateY:
        return Kind::RotateY;
    case TransformFunction::RotateZ:
        return Kind::RotateZ;
    case TransformFunction::Skew:
        return Kind::Skew;
    case TransformFunction::SkewX:
        return Kind::SkewX;
    case TransformFunction::SkewY:
        return Kind::SkewY;
    case TransformFunction::Matrix:
    case TransformFunction::Matrix3d:
    case TransformFunction::Perspective:
    case TransformFunction::Rotate3d:
        return {};
    }
    VERIFY_NOT_REACHED();
}

static Optional<CSS::Length> compositor_transform_percentage_basis(CSS::TransformFunction function, size_t argument_index, CSSPixelSize reference_size)
{
    switch (function) {
    case CSS::TransformFunction::Translate:
    case CSS::TransformFunction::Translate3d:
        if (argument_index == 0)
            return CSS::Length::make_px(reference_size.width());
        if (argument_index == 1)
            return CSS::Length::make_px(reference_size.height());
        return {};
    case CSS::TransformFunction::TranslateX:
        return CSS::Length::make_px(reference_size.width());
    case CSS::TransformFunction::TranslateY:
        return CSS::Length::make_px(reference_size.height());
    default:
        return {};
    }
}

static Optional<Compositor::VisualAnimationTransformOperation> compositor_transform_operation(CSS::TransformationStyleValue const& transformation, CSSPixelSize reference_size, float device_pixels_per_css_pixel)
{
    auto kind = compositor_transform_operation_kind(transformation.transform_function());
    if (!kind.has_value())
        return {};

    auto metadata = CSS::transform_function_metadata(transformation.transform_function());
    auto style_values = transformation.values();
    Vector<float> values;
    values.ensure_capacity(style_values.size());
    for (size_t index = 0; index < style_values.size(); ++index) {
        auto const& style_value = style_values[index];
        auto value = [&]() -> float {
            switch (metadata.parameters[index].type) {
            case CSS::TransformFunctionParameterType::Angle:
                return CSS::Angle::from_style_value(style_value, {}).to_radians();
            case CSS::TransformFunctionParameterType::Length:
            case CSS::TransformFunctionParameterType::LengthNone:
            case CSS::TransformFunctionParameterType::LengthPercentage:
                return CSS::Length::from_style_value(style_value, compositor_transform_percentage_basis(transformation.transform_function(), index, reference_size)).absolute_length_to_px().to_float() * device_pixels_per_css_pixel;
            case CSS::TransformFunctionParameterType::Number:
            case CSS::TransformFunctionParameterType::NumberPercentage:
                return CSS::number_from_style_value(style_value, 1);
            }
            VERIFY_NOT_REACHED();
        }();
        if (!isfinite(value))
            return {};
        values.unchecked_append(value);
    }
    if (*kind == Compositor::VisualAnimationTransformOperationKind::Translate && values.size() == 1)
        kind = Compositor::VisualAnimationTransformOperationKind::TranslateX;
    Compositor::VisualAnimationTransformOperation operation { *kind, move(values) };
    if (!operation.is_valid())
        return {};
    return operation;
}

static Optional<Compositor::VisualAnimationEasing> compositor_animation_easing(Animations::KeyframeEffect::KeyFrameSet::ResolvedKeyFrame const& keyframe, Animations::Animation const& animation)
{
    return keyframe.easing.visit(
        [&](Empty) -> Optional<Compositor::VisualAnimationEasing> {
            auto easing = animation.is_css_animation()
                ? static_cast<CSS::CSSAnimation const&>(animation).default_easing()
                : CSS::EasingFunction::linear();
            return Compositor::VisualAnimationEasing::from_css(easing);
        },
        [](CSS::EasingFunction const& easing) -> Optional<Compositor::VisualAnimationEasing> {
            return Compositor::VisualAnimationEasing::from_css(easing);
        },
        [](CSS::RustStyleValueHandle const&) -> Optional<Compositor::VisualAnimationEasing> {
            return {};
        });
}

static RefPtr<CSS::StyleValue const> resolved_compositor_animation_style_value(CSS::PropertyID property_id, CSS::RustStyleValueHandle const& value, DOM::AbstractElement target)
{
    ++target.document().style_invalidation_counters().compositor_keyframe_value_resolutions;
    auto style_value = CSS::StyleValue::adopt_rust_style_value_data(CSS::StyleValueFFI::rust_style_value_retain(value.data()));
    if (style_value->is_unresolved())
        style_value = target.document().style_computer().resolve_unresolved_style_value(target, CSS::PropertyNameAndID::from_id(property_id), style_value->as_unresolved());
    if (style_value->is_guaranteed_invalid() || style_value->is_unresolved() || style_value->is_pending_substitution())
        return nullptr;
    CSS::ComputationContext computation_context {
        .length_resolution_context = CSS::Length::ResolutionContext::for_element(target),
        .abstract_element = target,
    };
    return style_value->absolutized(computation_context);
}

static bool is_transform_family_property(CSS::PropertyID property_id)
{
    return first_is_one_of(property_id,
        CSS::PropertyID::Translate,
        CSS::PropertyID::Rotate,
        CSS::PropertyID::Scale,
        CSS::PropertyID::Transform);
}

static Optional<Compositor::VisualAnimationTransformList> compositor_transform_animation_value(CSS::PropertyID property_id, CSS::StyleValue const& style_value, Layout::Node const& layout_node, float device_pixels_per_css_pixel)
{
    Vector<NonnullRefPtr<CSS::TransformationStyleValue const>> transformations;
    if (property_id == CSS::PropertyID::Transform) {
        transformations = CSS::transformations_for_style_value(style_value);
    } else {
        if (!style_value.is_transformation())
            return {};
        transformations.append(style_value.as_transformation());
    }
    if (transformations.is_empty())
        return {};

    Compositor::VisualAnimationTransformList operations;
    operations.ensure_capacity(transformations.size());
    for (auto const& transformation : transformations) {
        auto operation = compositor_transform_operation(*transformation, Painting::transform_reference_box(layout_node).size(), device_pixels_per_css_pixel);
        if (!operation.has_value())
            return {};
        operations.unchecked_append(operation.release_value());
    }
    return operations;
}

static Optional<float> compositor_opacity_animation_value(CSS::StyleValue const& style_value)
{
    if (!style_value.is_opacity_value())
        return {};
    auto opacity = style_value.as_opacity_value().resolved();
    if (!isfinite(opacity))
        return {};
    return opacity;
}

static bool transform_keyframes_only_translate_horizontally(ReadonlySpan<Compositor::VisualAnimationKeyframe> keyframes)
{
    Vector<float> translate_y_values;
    Vector<Compositor::VisualAnimationTransformOperationKind> operation_kinds;
    for (size_t keyframe_index = 0; keyframe_index < keyframes.size(); ++keyframe_index) {
        auto const& operations = keyframes[keyframe_index].value.get<Compositor::VisualAnimationTransformList>();
        if (keyframe_index != 0 && operations.size() != operation_kinds.size())
            return false;
        for (size_t operation_index = 0; operation_index < operations.size(); ++operation_index) {
            auto const& operation = operations[operation_index];
            if (keyframe_index == 0)
                operation_kinds.append(operation.kind);
            else if (operation.kind != operation_kinds[operation_index])
                return false;
            if (operation.kind == Compositor::VisualAnimationTransformOperationKind::TranslateX) {
                if (keyframe_index == 0)
                    translate_y_values.append(0);
                continue;
            }
            if (operation.kind != Compositor::VisualAnimationTransformOperationKind::Translate)
                return false;
            float translate_y = operation.values.size() == 2 ? operation.values[1] : 0;
            if (keyframe_index == 0)
                translate_y_values.append(translate_y);
            else if (operation_index >= translate_y_values.size() || translate_y_values[operation_index] != translate_y)
                return false;
        }
    }
    return keyframes.size() >= 2;
}

static bool keyframe_effect_only_translates_horizontally(Animations::KeyframeEffect& effect, DOM::AbstractElement target, Layout::Node const& layout_node, float device_pixels_per_css_pixel)
{
    if (effect.target_properties().size() != 1 || !effect.target_properties().contains(CSS::PropertyNameAndID::from_id(CSS::PropertyID::Transform)))
        return false;
    auto const* key_frame_set = effect.key_frame_set();
    if (!key_frame_set)
        return false;

    Vector<Compositor::VisualAnimationKeyframe> keyframes;
    for (auto const& entry : key_frame_set->keyframes_by_key) {
        auto property = entry.properties.get(CSS::PropertyNameAndID::from_id(CSS::PropertyID::Transform));
        if (!property.has_value())
            continue;
        if (!property->has<CSS::RustStyleValueHandle>())
            return false;
        auto style_value = resolved_compositor_animation_style_value(CSS::PropertyID::Transform, property->get<CSS::RustStyleValueHandle>(), target);
        if (!style_value)
            return false;
        auto operations = compositor_transform_animation_value(CSS::PropertyID::Transform, *style_value, layout_node, device_pixels_per_css_pixel);
        if (!operations.has_value())
            return false;
        keyframes.append({
            .offset = 0,
            .easing = {},
            .value = operations.release_value(),
        });
    }
    return transform_keyframes_only_translate_horizontally(keyframes);
}

static Optional<Compositor::VisualAnimation> build_compositor_animation(Animations::KeyframeEffect& effect, Painting::AccumulatedVisualContextTree const& visual_context_tree, Compositor::VisualAnimation::TargetKind target_kind, Optional<bool>& only_translates_horizontally)
{
    auto animation = effect.associated_animation();
    if (!animation || animation->play_state() != Bindings::AnimationPlayState::Running || animation->pending())
        return {};
    auto timeline = animation->timeline();
    if (!timeline || !timeline->is_monotonically_increasing() || !effect.is_in_the_active_phase())
        return {};
    if ((!isinf(effect.iteration_count()) && effect.iteration_count() != 1) || effect.composite() != Bindings::CompositeOperation::Replace)
        return {};
    if (animation->playback_rate() <= 0 || !isfinite(animation->playback_rate()))
        return {};
    if (effect.start_delay().type != Animations::TimeValue::Type::Milliseconds
        || effect.iteration_duration().type != Animations::TimeValue::Type::Milliseconds
        || effect.iteration_duration().value <= 0)
        return {};
    auto current_time = animation->current_time();
    if (!current_time.has_value() || current_time->type != Animations::TimeValue::Type::Milliseconds)
        return {};
    if (effect.target_properties().is_empty())
        return {};

    bool targets_opacity = target_kind == Compositor::VisualAnimation::TargetKind::Opacity && effect.target_properties().contains(CSS::PropertyNameAndID::from_id(CSS::PropertyID::Opacity));
    bool targets_transform = target_kind == Compositor::VisualAnimation::TargetKind::Transform && any_of(effect.target_properties(), [](auto const& property) { return is_transform_family_property(property.id()); });
    if (!targets_opacity && !targets_transform)
        return {};
    if (any_of(effect.target_properties(), [&](auto const& property) { return property.id() != CSS::PropertyID::Opacity && !is_transform_family_property(property.id()); }))
        return {};
    auto target = effect.target_abstract_element();
    if (!target.has_value() || target->element().namespace_uri() == Namespace::SVG)
        return {};
    auto frame_timestamp = target->document().last_animation_frame_timestamp();
    if (!frame_timestamp.has_value())
        return {};
    auto monotonic_time_at_anchor_ms = *frame_timestamp + target->document().relevant_settings_object().time_origin();
    auto const* layout_node = target->unsafe_layout_node();
    if (!layout_node)
        return {};
    if (targets_transform) {
        if ((!effect.target_properties().contains(CSS::PropertyNameAndID::from_id(CSS::PropertyID::Translate)) && layout_node->has_translate())
            || (!effect.target_properties().contains(CSS::PropertyNameAndID::from_id(CSS::PropertyID::Rotate)) && layout_node->has_rotate())
            || (!effect.target_properties().contains(CSS::PropertyNameAndID::from_id(CSS::PropertyID::Scale)) && layout_node->has_scale())
            || (!effect.target_properties().contains(CSS::PropertyNameAndID::from_id(CSS::PropertyID::Transform)) && layout_node->has_transformations())
            || layout_node->transform_origin().z.to_px(CSSPixels { 0 }) != CSSPixels { 0 })
            return {};
    }
    auto const* row = Painting::committed_row(*layout_node);
    if (!row)
        return {};

    auto const* key_frame_set = effect.key_frame_set();
    if (!key_frame_set || key_frame_set->keyframes_by_key.size() < 2)
        return {};
    auto device_pixels_per_css_pixel = static_cast<float>(target->document().page().client().device_pixels_per_css_pixel());
    auto reference_box_size = Painting::transform_reference_box(*layout_node).size();
    auto resolved_values_for_target = [&](Compositor::VisualAnimation::TargetKind target_kind) -> Animations::KeyframeEffect::CompositorKeyframeValueCache const& {
        auto& cached_values = effect.compositor_keyframe_value_cache(target_kind);
        auto reference_width = target_kind == Compositor::VisualAnimation::TargetKind::Transform ? reference_box_size.width().to_float() : 0;
        auto reference_height = target_kind == Compositor::VisualAnimation::TargetKind::Transform ? reference_box_size.height().to_float() : 0;
        auto target_style_generation = target->element().animation_style_generation();
        auto style_environment_version = target->document().style_environment_version();
        if (cached_values.has_value()
            && cached_values->key_frame_set == key_frame_set
            && cached_values->target_style_generation == target_style_generation
            && cached_values->style_environment_version == style_environment_version
            && cached_values->reference_width == reference_width
            && cached_values->reference_height == reference_height
            && cached_values->device_pixels_per_css_pixel == device_pixels_per_css_pixel)
            return *cached_values;

        Animations::KeyframeEffect::CompositorKeyframeValueCache new_cache {
            .key_frame_set = key_frame_set,
            .target_style_generation = target_style_generation,
            .style_environment_version = style_environment_version,
            .reference_width = reference_width,
            .reference_height = reference_height,
            .device_pixels_per_css_pixel = device_pixels_per_css_pixel,
            .is_valid = true,
            .values = {},
        };
        new_cache.values.ensure_capacity(key_frame_set->keyframes_by_key.size());
        size_t transform_property_count = 0;
        for (auto const& property : effect.target_properties()) {
            if (is_transform_family_property(property.id()))
                ++transform_property_count;
        }
        for (auto const& entry : key_frame_set->keyframes_by_key) {
            Optional<Compositor::VisualAnimationValue> value;
            if (target_kind == Compositor::VisualAnimation::TargetKind::Opacity) {
                auto property = entry.properties.get(CSS::PropertyNameAndID::from_id(CSS::PropertyID::Opacity));
                if (!property.has_value() || !property->has<CSS::RustStyleValueHandle>()) {
                    new_cache.values.append({});
                    continue;
                }
                auto style_value = resolved_compositor_animation_style_value(CSS::PropertyID::Opacity, property->get<CSS::RustStyleValueHandle>(), *target);
                if (style_value) {
                    auto opacity = compositor_opacity_animation_value(*style_value);
                    if (opacity.has_value())
                        value = Compositor::VisualAnimationValue { opacity.release_value() };
                }
            } else {
                Compositor::VisualAnimationTransformList operations;
                bool skip_keyframe = false;
                for (auto property_id : { CSS::PropertyID::Translate, CSS::PropertyID::Rotate, CSS::PropertyID::Scale, CSS::PropertyID::Transform }) {
                    if (!effect.target_properties().contains(CSS::PropertyNameAndID::from_id(property_id)))
                        continue;
                    auto property = entry.properties.get(CSS::PropertyNameAndID::from_id(property_id));
                    if (!property.has_value() || !property->has<CSS::RustStyleValueHandle>()) {
                        if (transform_property_count == 1) {
                            skip_keyframe = true;
                            break;
                        }
                        new_cache.is_valid = false;
                        break;
                    }
                    auto style_value = resolved_compositor_animation_style_value(property_id, property->get<CSS::RustStyleValueHandle>(), *target);
                    if (!style_value) {
                        new_cache.is_valid = false;
                        break;
                    }
                    auto property_operations = compositor_transform_animation_value(property_id, *style_value, *layout_node, device_pixels_per_css_pixel);
                    if (!property_operations.has_value()) {
                        new_cache.is_valid = false;
                        break;
                    }
                    operations.extend(property_operations.release_value());
                }
                if (!new_cache.is_valid)
                    break;
                if (skip_keyframe) {
                    new_cache.values.append({});
                    continue;
                }
                value = Compositor::VisualAnimationValue { move(operations) };
            }
            if (!value.has_value()) {
                new_cache.is_valid = false;
                break;
            }
            new_cache.values.append(value.release_value());
        }
        cached_values = move(new_cache);
        return *cached_values;
    };
    auto build_animation_for_target = [&](Compositor::VisualAnimation::TargetKind target_kind) -> Optional<Compositor::VisualAnimation> {
        auto visual_context_node_indices = Painting::rust_visual_animation_target_node_indices(*layout_node, visual_context_tree, target_kind == Compositor::VisualAnimation::TargetKind::Opacity);
        if (visual_context_node_indices.is_empty())
            return {};

        Vector<Compositor::VisualAnimationEasing> keyframe_easings;
        keyframe_easings.ensure_capacity(key_frame_set->keyframes_by_key.size());
        for (auto it = key_frame_set->keyframes_by_key.begin(); it != key_frame_set->keyframes_by_key.end(); ++it) {
            auto const& entry = *it;
            auto composite = [&] {
                switch (entry.composite) {
                case Bindings::CompositeOperationOrAuto::Replace:
                    return Bindings::CompositeOperation::Replace;
                case Bindings::CompositeOperationOrAuto::Add:
                    return Bindings::CompositeOperation::Add;
                case Bindings::CompositeOperationOrAuto::Accumulate:
                    return Bindings::CompositeOperation::Accumulate;
                case Bindings::CompositeOperationOrAuto::Auto:
                    return effect.composite();
                }
                VERIFY_NOT_REACHED();
            }();
            if (composite != Bindings::CompositeOperation::Replace)
                return {};
            auto easing = compositor_animation_easing(entry, *animation);
            if (!easing.has_value())
                return {};
            keyframe_easings.append(easing.release_value());
        }

        auto const& cached_values = resolved_values_for_target(target_kind);
        if (!cached_values.is_valid)
            return {};
        VERIFY(cached_values.values.size() == key_frame_set->keyframes_by_key.size());

        Vector<Compositor::VisualAnimationKeyframe> keyframes;
        size_t keyframe_index = 0;
        for (auto it = key_frame_set->keyframes_by_key.begin(); it != key_frame_set->keyframes_by_key.end(); ++it, ++keyframe_index) {
            auto const& value = cached_values.values[keyframe_index];
            if (!value.has_value())
                continue;
            keyframes.append({
                .offset = static_cast<double>(it.key()) / (100.0 * Animations::KeyframeEffect::AnimationKeyFrameKeyScaleFactor),
                .easing = keyframe_easings[keyframe_index],
                .value = *value,
            });
        }

        if (target_kind == Compositor::VisualAnimation::TargetKind::Transform) {
            only_translates_horizontally = effect.target_properties().size() == 1
                && effect.target_properties().contains(CSS::PropertyNameAndID::from_id(CSS::PropertyID::Transform))
                && transform_keyframes_only_translate_horizontally(keyframes);
        }

        Compositor::VisualAnimation visual_animation {
            .target_kind = target_kind,
            .visual_context_node_indices = move(visual_context_node_indices),
            .monotonic_time_at_anchor_ns = static_cast<i64>(monotonic_time_at_anchor_ms * 1'000'000.0),
            .local_time_at_anchor_ms = current_time->value,
            .playback_rate = animation->playback_rate(),
            .start_delay_ms = effect.start_delay().value,
            .iteration_duration_ms = effect.iteration_duration().value,
            .iteration_count = effect.iteration_count(),
            .iteration_start = effect.iteration_start(),
            .playback_direction = static_cast<Compositor::VisualAnimationPlaybackDirection>(to_underlying(effect.playback_direction())),
            .easing = Compositor::VisualAnimationEasing::from_css(effect.timing_function()),
            .keyframes = move(keyframes),
        };
        if (!visual_animation.is_valid())
            return {};
        return visual_animation;
    };

    return build_animation_for_target(target_kind);
}

void Document::schedule_compositor_animation_wakeup(double delay_ms)
{
    auto timer_delay_ms = clamp(static_cast<i64>(ceil(delay_ms)), 1, static_cast<i64>(NumericLimits<int>::max()));
    auto deadline = MonotonicTime::now() + AK::Duration::from_milliseconds(timer_delay_ms);
    if (m_compositor_animation_wakeup_timer && m_compositor_animation_wakeup_timer->is_active()
        && m_compositor_animation_wakeup_deadline.has_value() && *m_compositor_animation_wakeup_deadline <= deadline)
        return;

    m_compositor_animation_wakeup_deadline = deadline;
    if (!m_compositor_animation_wakeup_timer) {
        m_compositor_animation_wakeup_timer = Core::Timer::create_single_shot(static_cast<int>(timer_delay_ms), GC::weak_callback(*this, [](auto& document) {
            // Cancelling a finite effect can leave this deadline armed. The single wake-up is harmless: it finds
            // nothing due and leaves the timer idle unless another effect supplies a deadline.
            document.m_compositor_animation_wakeup_deadline.clear();
            auto timestamp = HighResolutionTime::relative_high_resolution_time(
                HighResolutionTime::unsafe_shared_current_time(), document.relevant_settings_object().global_object());
            Optional<double> next_wakeup_delay_ms;
            bool reached_wakeup = false;
            for (auto& animation : document.m_associated_animations) {
                if (!animation.effect() || !is<Animations::KeyframeEffect>(*animation.effect()))
                    continue;
                auto& effect = static_cast<Animations::KeyframeEffect&>(*animation.effect());
                auto timeline_time = animation.timeline() ? animation.timeline()->current_time_at_timestamp(timestamp) : Optional<Animations::TimeValue> {};
                auto current_time = animation.current_time_at(timeline_time);
                if (!current_time.has_value() || current_time->type != Animations::TimeValue::Type::Milliseconds)
                    continue;
                if (!effect.is_compositor_driven()
                    && !animation.pending()
                    && animation.playback_rate() > 0
                    && effect.start_delay().type == Animations::TimeValue::Type::Milliseconds) {
                    if (current_time->value < effect.start_delay().value) {
                        auto delay = (effect.start_delay().value - current_time->value) / animation.playback_rate();
                        if (!next_wakeup_delay_ms.has_value() || delay < *next_wakeup_delay_ms)
                            next_wakeup_delay_ms = delay;
                        continue;
                    }
                    effect.request_observation_sample();
                    reached_wakeup = true;
                    continue;
                }
                bool is_compositor_handled = effect.is_compositor_driven()
                    || effect.is_compositor_replaced()
                    || !effect.retained_compositor_animations().is_empty();
                if (!is_compositor_handled || isinf(effect.iteration_count()))
                    continue;
                auto active_end = effect.start_delay().value + effect.iteration_duration().value * effect.iteration_count();
                if (current_time->value < active_end) {
                    auto delay = (active_end - current_time->value) / animation.playback_rate();
                    if (!next_wakeup_delay_ms.has_value() || delay < *next_wakeup_delay_ms)
                        next_wakeup_delay_ms = delay;
                    continue;
                }
                effect.request_observation_sample();
                reached_wakeup = true;
            }
            if (next_wakeup_delay_ms.has_value())
                document.schedule_compositor_animation_wakeup(*next_wakeup_delay_ms);
            if (reached_wakeup) {
                ++document.m_style_invalidation_counters.animation_frame_pump_requests;
                document.page().client().request_frame();
            }
        }));
    }
    m_compositor_animation_wakeup_timer->restart(static_cast<int>(timer_delay_ms));
}

void Document::update_compositor_animations()
{
    struct CompetingPropertyEffects {
        GC::Ptr<Animations::KeyframeEffect> winner;
        bool all_effects_use_replace { true };
    };
    struct CompetingEffects {
        CompetingPropertyEffects opacity;
        CompetingPropertyEffects transform;
    };

    auto visual_context_tree = paint_state().visual_context_tree(*this);
    Vector<Compositor::VisualAnimation> visual_animations;
    GC::RootHashTable<GC::Ref<Animations::KeyframeEffect>> previously_compositor_driven_effects;
    GC::RootHashTable<GC::Ref<Animations::KeyframeEffect>> previously_compositor_replaced_effects;
    GC::RootHashTable<GC::Ref<Animations::KeyframeEffect>> previously_published_effects;
    GC::RootHashTable<GC::Ref<Animations::KeyframeEffect>> previously_offscreen_throttled_effects;
    HashMap<DOM::AbstractElement, CompetingEffects> competing_effects;
    HashMap<GC::Ptr<Animations::KeyframeEffect>, bool> only_translates_horizontally_cache;
    HashMap<GC::Ptr<Animations::KeyframeEffect>, bool> animated_transform_preserves_axes_cache;
    HashMap<Element const*, Vector<GC::Ptr<Animations::KeyframeEffect>>> in_effect_transform_effects_by_target;
    HashMap<GC::Ptr<Element>, CSSPixelRect> observation_target_rect_cache;
    GC::RootHashTable<GC::Ref<Element>> elements_with_intersection_observation_descendants;
    GC::RootHashTable<GC::Ref<Element>> elements_with_visibility_observation_descendants;
    Optional<double> compositor_animation_wakeup_delay_ms;

    auto index_shadow_including_element_ancestors = [](Node& node, GC::RootHashTable<GC::Ref<Element>>& index) {
        for (auto* ancestor = &node; ancestor; ancestor = ancestor->parent_or_shadow_host()) {
            if (auto* element = as_if<Element>(*ancestor))
                index.set(*element);
        }
    };
    for (auto const& observer : m_intersection_observers) {
        if (observer.observation_targets().is_empty())
            continue;
        auto root = observer.intersection_root_node();
        if (root->is_element())
            index_shadow_including_element_ancestors(*root, elements_with_intersection_observation_descendants);
        for (auto const& observation : observer.observation_targets()) {
            index_shadow_including_element_ancestors(observation.target, elements_with_intersection_observation_descendants);
            if (observer.track_visibility())
                index_shadow_including_element_ancestors(observation.target, elements_with_visibility_observation_descendants);
        }
    }

    auto transform_preserves_horizontal_axis = [](Layout::NodeWithStyle const& layout_node) {
        if (layout_node.perspective().has_value())
            return false;

        auto matrix = Gfx::FloatMatrix4x4::identity();
        if (auto translate = layout_node.translate())
            matrix = matrix * translate->to_matrix(&layout_node);
        if (auto rotate = layout_node.rotate())
            matrix = matrix * rotate->to_matrix(&layout_node);
        if (auto scale = layout_node.scale())
            matrix = matrix * scale->to_matrix(&layout_node);
        layout_node.for_each_transformation([&](auto const& transformation) {
            matrix = matrix * transformation.to_matrix(&layout_node);
        });

        constexpr auto epsilon = AK::NumericLimits<float>::epsilon();
        return abs(matrix[1, 0]) <= epsilon
            && abs(matrix[2, 0]) <= epsilon
            && abs(matrix[3, 0]) <= epsilon;
    };

    auto animated_transform_preserves_axes = [&](Animations::KeyframeEffect& effect, Element& target, Layout::Node const& layout_node) {
        return animated_transform_preserves_axes_cache.ensure(effect, [&] {
            if (effect.target_properties().contains(CSS::PropertyNameAndID::from_id(CSS::PropertyID::Rotate)))
                return false;
            if (!effect.target_properties().contains(CSS::PropertyNameAndID::from_id(CSS::PropertyID::Transform)))
                return true;
            auto const* key_frame_set = effect.key_frame_set();
            if (!key_frame_set)
                return false;
            auto device_pixels_per_css_pixel = static_cast<float>(page().client().device_pixels_per_css_pixel());
            for (auto const& entry : key_frame_set->keyframes_by_key) {
                auto property = entry.properties.get(CSS::PropertyNameAndID::from_id(CSS::PropertyID::Transform));
                if (!property.has_value())
                    continue;
                if (!property->has<CSS::RustStyleValueHandle>())
                    return false;
                auto style_value = resolved_compositor_animation_style_value(CSS::PropertyID::Transform, property->get<CSS::RustStyleValueHandle>(), target);
                if (!style_value)
                    return false;
                auto operations = compositor_transform_animation_value(CSS::PropertyID::Transform, *style_value, layout_node, device_pixels_per_css_pixel);
                if (!operations.has_value())
                    return false;
                if (any_of(*operations, [](auto const& operation) {
                        return !first_is_one_of(operation.kind,
                            Compositor::VisualAnimationTransformOperationKind::Translate,
                            Compositor::VisualAnimationTransformOperationKind::Translate3d,
                            Compositor::VisualAnimationTransformOperationKind::TranslateX,
                            Compositor::VisualAnimationTransformOperationKind::TranslateY,
                            Compositor::VisualAnimationTransformOperationKind::TranslateZ,
                            Compositor::VisualAnimationTransformOperationKind::Scale,
                            Compositor::VisualAnimationTransformOperationKind::Scale3d,
                            Compositor::VisualAnimationTransformOperationKind::ScaleX,
                            Compositor::VisualAnimationTransformOperationKind::ScaleY,
                            Compositor::VisualAnimationTransformOperationKind::ScaleZ);
                    }))
                    return false;
            }
            return true;
        });
    };

    auto ancestor_transform_can_map_horizontal_motion_to_vertical = [&](Element const& animated_target, Node const& observation_root) {
        auto const* layout_node = animated_target.unsafe_layout_node();
        if (!layout_node)
            return true;
        for (auto const* ancestor = layout_node->parent(); ancestor; ancestor = ancestor->parent()) {
            if (ancestor->has_css_transform() && !transform_preserves_horizontal_axis(*ancestor))
                return true;
            if (auto* ancestor_element = as_if<Element>(ancestor->dom_node())) {
                if (auto effects = in_effect_transform_effects_by_target.find(ancestor_element); effects != in_effect_transform_effects_by_target.end()) {
                    for (auto effect : effects->value) {
                        if (!animated_transform_preserves_axes(*effect, const_cast<Element&>(*ancestor_element), *ancestor))
                            return true;
                    }
                }
            }
            if (ancestor->dom_node() == &observation_root)
                break;
        }
        return false;
    };

    auto transform_subtree_is_clipped_outside = [](Element const& animated_target, CSSPixelRect const& root_bounds) {
        auto const* layout_node = animated_target.unsafe_layout_node();
        if (!layout_node || !Painting::has_committed_box(*layout_node) || root_bounds.is_empty())
            return false;
        if (auto const* target_box = as_if<Layout::Box>(*layout_node)) {
            if (Painting::is_fixed_position(*target_box) || target_box->abspos_descendant_escapes())
                return false;
        }

        bool has_disjoint_clip = false;
        for (auto const* ancestor = layout_node->parent(); ancestor; ancestor = ancestor->parent()) {
            auto const* ancestor_box = as_if<Layout::Box>(*ancestor);
            if (!ancestor_box)
                continue;
            if (!Painting::has_committed_box(*ancestor_box) || Painting::is_fixed_position(*ancestor_box))
                return false;

            bool has_content_clip = ancestor_box->overflow_x() != CSS::Overflow::Visible
                || ancestor_box->overflow_y() != CSS::Overflow::Visible;
            if (has_content_clip) {
                auto clip_rect = Painting::transform_rect_to_viewport(*ancestor_box, Painting::absolute_padding_box_rect(*ancestor_box), Painting::AccumulatedVisualContextTree::IncludeVisualViewportTransform::No);
                if (!clip_rect.edge_adjacent_intersects(root_bounds))
                    has_disjoint_clip = true;
            }

            // A transform outside the disjoint clip could move the clip into the observation root without updating
            // its main-thread geometry. Transforms inside the clip can only move content within the clipped region.
            if (has_disjoint_clip && (ancestor_box->has_css_transform() || ancestor_box->is_sticky_position()))
                return false;
        }
        return has_disjoint_clip;
    };

    auto observation_has_another_transform_animation = [&](Element const& animated_target, Element const& observation_target, Animations::KeyframeEffect const& current_effect) {
        for (auto& animation : m_associated_animations) {
            if (animation.is_idle() || !animation.effect() || !is<Animations::KeyframeEffect>(*animation.effect()))
                continue;
            auto const& effect = static_cast<Animations::KeyframeEffect const&>(*animation.effect());
            if (&effect == &current_effect || !effect.is_in_effect())
                continue;
            auto effect_target = effect.target();
            if (!effect_target || !animated_target.is_shadow_including_inclusive_ancestor_of(*effect_target)
                || !effect_target->is_shadow_including_inclusive_ancestor_of(observation_target))
                continue;
            if (any_of(effect.target_properties(), [](auto const& property) { return is_transform_family_property(property.id()); }))
                return true;
        }
        return false;
    };

    auto transform_affects_intersection_observation = [&](Element& animated_target, Animations::KeyframeEffect const& effect, bool only_translates_horizontally, bool& requires_main_thread_sampling) {
        if (!elements_with_intersection_observation_descendants.contains(animated_target))
            return false;
        for (auto const& observer : m_intersection_observers) {
            if (observer.observation_targets().is_empty())
                continue;
            auto root = observer.intersection_root_node();
            if (root->is_element() && animated_target.is_shadow_including_inclusive_ancestor_of(*root))
                return true;
            bool subtree_is_clipped_outside_root = transform_subtree_is_clipped_outside(animated_target, observer.root_intersection_rectangle());
            for (auto const& observation : observer.observation_targets()) {
                if (!animated_target.is_shadow_including_inclusive_ancestor_of(*observation.target) || subtree_is_clipped_outside_root)
                    continue;
                if (only_translates_horizontally && ancestor_transform_can_map_horizontal_motion_to_vertical(animated_target, *root)) {
                    requires_main_thread_sampling = true;
                    return true;
                }
                if (only_translates_horizontally && !observation_has_another_transform_animation(animated_target, *observation.target, effect)) {
                    auto const& target_rect = observation_target_rect_cache.ensure(observation.target, [&] {
                        return observation.target->bounding_client_rect_assuming_layout_clean();
                    });
                    auto root_rect = observer.root_intersection_rectangle();
                    bool vertical_bands_are_disjoint = target_rect.bottom() < root_rect.top() || target_rect.top() > root_rect.bottom();
                    if (vertical_bands_are_disjoint)
                        continue;
                }
                return true;
            }
        }
        return false;
    };

    auto opacity_affects_visibility_observation = [&](Element& animated_target) {
        return elements_with_visibility_observation_descendants.contains(animated_target);
    };

    for (auto& animation : m_associated_animations) {
        if (!animation.effect() || !is<Animations::KeyframeEffect>(*animation.effect()))
            continue;
        auto& effect = static_cast<Animations::KeyframeEffect&>(*animation.effect());
        if (effect.is_compositor_driven())
            previously_compositor_driven_effects.set(effect);
        if (effect.is_compositor_replaced())
            previously_compositor_replaced_effects.set(effect);
        if (!effect.retained_compositor_animations().is_empty())
            previously_published_effects.set(effect);
        if (effect.is_offscreen_throttled())
            previously_offscreen_throttled_effects.set(effect);
        if (auto target = effect.target_abstract_element(); target.has_value()) {
            if (auto* layout_node = target->unsafe_layout_node())
                layout_node->set_retains_compositor_animated_content(false);
        }
        effect.set_is_compositor_driven(false);
        effect.set_is_compositor_replaced(false);
        effect.set_is_offscreen_throttled(false);
        effect.set_is_observation_relevant_compositor_animation(false);

        if (animation.is_idle() || !effect.is_in_effect())
            continue;
        auto target = effect.target_abstract_element();
        if (!target.has_value())
            continue;
        auto& effects = competing_effects.ensure(*target);
        auto add_competing_effect = [&](CompetingPropertyEffects& property_effects) {
            if (effect.composite() != Bindings::CompositeOperation::Replace)
                property_effects.all_effects_use_replace = false;
            if (!property_effects.winner || Animations::KeyframeEffect::composite_order(*property_effects.winner, effect) < 0)
                property_effects.winner = effect;
        };
        if (effect.target_properties().contains(CSS::PropertyNameAndID::from_id(CSS::PropertyID::Opacity)))
            add_competing_effect(effects.opacity);
        if (any_of(effect.target_properties(), [](auto const& property) { return is_transform_family_property(property.id()); })) {
            add_competing_effect(effects.transform);
            in_effect_transform_effects_by_target.ensure(&target->element()).append(effect);
        }
    }

    auto winner_uses_replace_keyframes = [](Animations::KeyframeEffect& effect) {
        auto const* key_frame_set = effect.key_frame_set();
        if (!key_frame_set)
            return false;
        return all_of(key_frame_set->keyframes_by_key, [](auto const& entry) {
            return first_is_one_of(entry.composite,
                Bindings::CompositeOperationOrAuto::Auto,
                Bindings::CompositeOperationOrAuto::Replace);
        });
    };
    for (auto& [target, effects] : competing_effects) {
        (void)target;
        auto validate_winner = [&](CompetingPropertyEffects& property_effects) {
            if (!property_effects.all_effects_use_replace
                || (property_effects.winner && !winner_uses_replace_keyframes(*property_effects.winner)))
                property_effects.winner = nullptr;
        };
        validate_winner(effects.opacity);
        validate_winner(effects.transform);
    }

    bool has_observation_relevant_compositor_animation = false;
    bool requested_withdrawn_effect_sample = false;
    auto schedule_active_end_wakeup = [&](Animations::KeyframeEffect const& effect, Animations::Animation const& animation) {
        if (isinf(effect.iteration_count())
            || effect.start_delay().type != Animations::TimeValue::Type::Milliseconds
            || effect.iteration_duration().type != Animations::TimeValue::Type::Milliseconds
            || !animation.current_time().has_value()
            || animation.current_time()->type != Animations::TimeValue::Type::Milliseconds
            || animation.playback_rate() <= 0)
            return;
        auto active_end = effect.start_delay().value + effect.iteration_duration().value * effect.iteration_count();
        auto delay = (active_end - animation.current_time()->value) / animation.playback_rate();
        if (delay > 0 && (!compositor_animation_wakeup_delay_ms.has_value() || delay < *compositor_animation_wakeup_delay_ms))
            compositor_animation_wakeup_delay_ms = delay;
    };
    for (auto& animation : m_associated_animations) {
        if (!animation.effect() || !is<Animations::KeyframeEffect>(*animation.effect()))
            continue;
        auto& effect = static_cast<Animations::KeyframeEffect&>(*animation.effect());
        bool published_compositor_animation = false;
        ScopeGuard collect_effect_bookkeeping = [&] {
            if (!published_compositor_animation)
                effect.clear_retained_compositor_animations();
            if (effect.is_observation_relevant_compositor_animation())
                has_observation_relevant_compositor_animation = true;
            bool was_throttled = previously_compositor_driven_effects.contains(GC::Ref { effect })
                || previously_compositor_replaced_effects.contains(GC::Ref { effect })
                || previously_offscreen_throttled_effects.contains(GC::Ref { effect });
            if (was_throttled && !effect.is_compositor_driven() && !effect.is_compositor_replaced() && !effect.is_offscreen_throttled()) {
                effect.request_observation_sample();
                requested_withdrawn_effect_sample = true;
            }
        };
        auto abstract_target = effect.target_abstract_element();
        if (!abstract_target.has_value() || effect.target_properties().is_empty())
            continue;
        if (animation.is_idle() || !effect.is_in_effect())
            continue;
        auto& target = abstract_target->element();

        bool targets_opacity = effect.target_properties().contains(CSS::PropertyNameAndID::from_id(CSS::PropertyID::Opacity));
        bool targets_transform = any_of(effect.target_properties(), [](auto const& property) { return is_transform_family_property(property.id()); });
        bool targets_unsupported_property = any_of(effect.target_properties(), [&](auto const& property) { return property.id() != CSS::PropertyID::Opacity && !is_transform_family_property(property.id()); });
        if (targets_unsupported_property)
            continue;
        auto target_effects = competing_effects.get(*abstract_target);
        VERIFY(target_effects.has_value());
        bool opacity_has_replace_winner = !targets_opacity || target_effects->opacity.winner;
        bool transform_has_replace_winner = !targets_transform || target_effects->transform.winner;
        bool selected_for_opacity = targets_opacity && target_effects->opacity.winner.ptr() == &effect;
        bool selected_for_transform = targets_transform && target_effects->transform.winner.ptr() == &effect;
        bool all_targeted_properties_have_replace_winners = opacity_has_replace_winner && transform_has_replace_winner;

        if (!selected_for_opacity && !selected_for_transform) {
            bool can_throttle_replaced_effect = animation.play_state() == Bindings::AnimationPlayState::Running
                && !animation.pending()
                && animation.playback_rate() > 0
                && animation.timeline() && animation.timeline()->is_monotonically_increasing()
                && effect.is_in_the_active_phase()
                && (isinf(effect.iteration_count()) || effect.iteration_count() == 1)
                && effect.start_delay().type == Animations::TimeValue::Type::Milliseconds
                && effect.iteration_duration().type == Animations::TimeValue::Type::Milliseconds;
            if (all_targeted_properties_have_replace_winners && can_throttle_replaced_effect) {
                effect.set_is_compositor_replaced(true);
                schedule_active_end_wakeup(effect, animation);
            }
            continue;
        }

        Optional<bool> only_translates_horizontally;
        Vector<Compositor::VisualAnimation> effect_visual_animations;
        bool opacity_was_handed_off = !selected_for_opacity;
        if (selected_for_opacity) {
            auto visual_animation = build_compositor_animation(effect, visual_context_tree, Compositor::VisualAnimation::TargetKind::Opacity, only_translates_horizontally);
            if (visual_animation.has_value()) {
                effect_visual_animations.append(visual_animation.release_value());
                opacity_was_handed_off = true;
            }
        }
        bool transform_was_handed_off = !selected_for_transform;
        if (selected_for_transform) {
            auto visual_animation = build_compositor_animation(effect, visual_context_tree, Compositor::VisualAnimation::TargetKind::Transform, only_translates_horizontally);
            if (visual_animation.has_value()) {
                effect_visual_animations.append(visual_animation.release_value());
                transform_was_handed_off = true;
            }
        }
        if (only_translates_horizontally.has_value())
            only_translates_horizontally_cache.set(effect, *only_translates_horizontally);

        if (selected_for_opacity && opacity_affects_visibility_observation(target)) {
            effect_visual_animations.remove_all_matching([](auto const& animation) {
                return animation.target_kind == Compositor::VisualAnimation::TargetKind::Opacity;
            });
            opacity_was_handed_off = false;
        }

        bool transform_affects_observation = false;
        bool requires_main_thread_observation_sampling = false;
        if (selected_for_transform) {
            auto only_translates_horizontally = only_translates_horizontally_cache.ensure(effect, [&] {
                auto const* layout_node = abstract_target->unsafe_layout_node();
                auto device_pixels_per_css_pixel = static_cast<float>(page().client().device_pixels_per_css_pixel());
                return layout_node && keyframe_effect_only_translates_horizontally(effect, *abstract_target, *layout_node, device_pixels_per_css_pixel);
            });
            transform_affects_observation = transform_affects_intersection_observation(target, effect, only_translates_horizontally, requires_main_thread_observation_sampling);
        }
        if (requires_main_thread_observation_sampling) {
            effect_visual_animations.remove_all_matching([](auto const& animation) {
                return animation.target_kind == Compositor::VisualAnimation::TargetKind::Transform;
            });
            transform_was_handed_off = false;
        }

        if (effect_visual_animations.is_empty()) {
            auto viewport_bounds = CSSPixelRect { { 0, 0 }, viewport_rect().size() };
            if (selected_for_transform && !transform_affects_observation && transform_subtree_is_clipped_outside(target, viewport_bounds))
                effect.set_is_offscreen_throttled(true);
            continue;
        }
        // OPTIMIZATION: Ordinary rendering updates advance the WebContent timeline without changing compositor
        //               playback. Retain the existing descriptor and its anchor unless the effect was invalidated or
        //               one of its non-anchor parameters changed.
        if (previously_published_effects.contains(GC::Ref { effect })) {
            for (auto& visual_animation : effect_visual_animations) {
                for (auto const& retained_animation : effect.retained_compositor_animations()) {
                    if (!visual_animation.has_same_animation_parameters(retained_animation))
                        continue;
                    visual_animation.monotonic_time_at_anchor_ns = retained_animation.monotonic_time_at_anchor_ns;
                    visual_animation.local_time_at_anchor_ms = retained_animation.local_time_at_anchor_ms;
                    break;
                }
            }
        }
        if (all_targeted_properties_have_replace_winners && opacity_was_handed_off && transform_was_handed_off)
            effect.set_is_compositor_driven(true);
        effect.set_is_observation_relevant_compositor_animation(transform_affects_observation);
        schedule_active_end_wakeup(effect, animation);
        for (auto const& visual_animation : effect_visual_animations)
            visual_animations.append(visual_animation);
        effect.set_retained_compositor_animations(move(effect_visual_animations));
        if (auto* layout_node = abstract_target->unsafe_layout_node())
            layout_node->set_retains_compositor_animated_content(true);
        published_compositor_animation = true;
    }

    paint_state().set_visual_animations(*this, move(visual_animations));

    if (compositor_animation_wakeup_delay_ms.has_value())
        schedule_compositor_animation_wakeup(*compositor_animation_wakeup_delay_ms);

    if (!has_observation_relevant_compositor_animation) {
        if (m_compositor_animation_observation_timer)
            m_compositor_animation_observation_timer->stop();
    } else {
        if (!m_compositor_animation_observation_timer) {
            m_compositor_animation_observation_timer = Core::Timer::create_single_shot(100, GC::weak_callback(*this, [](auto& document) {
                ++document.m_style_invalidation_counters.animation_frame_pump_requests;
                document.page().client().request_frame();
                document.m_compositor_animation_observation_timer->start();
            }));
        }
        if (!m_compositor_animation_observation_timer->is_active()) {
            m_compositor_animation_observation_timer->start();
        }
    }

    if (requested_withdrawn_effect_sample) {
        ++m_style_invalidation_counters.animation_frame_pump_requests;
    }
}

void Document::append_pending_animation_event(Web::DOM::Document::PendingAnimationEvent const& event)
{
    if (m_style_stabilization_epoch_depth > 0) {
        m_provisional_animation_event_queue.append(event);
        ++m_style_invalidation_counters.provisional_animation_events;
        return;
    }
    m_pending_animation_event_queue.append(event);
    ++m_style_invalidation_counters.committed_animation_events;
}

void Document::prepare_to_observe_css_animation_events()
{
    GC::RootVector<GC::Ref<Animations::KeyframeEffect>> effects_to_synchronize;
    for (auto& animation : m_associated_animations) {
        if (!animation.is_css_animation() || !animation.effect() || !is<Animations::KeyframeEffect>(*animation.effect()))
            continue;
        auto& effect = static_cast<Animations::KeyframeEffect&>(*animation.effect());
        if (effect.per_frame_animation_tick_was_skipped())
            effects_to_synchronize.append(effect);
    }
    if (effects_to_synchronize.is_empty())
        return;

    auto timestamp = HighResolutionTime::relative_high_resolution_time(
        HighResolutionTime::unsafe_shared_current_time(), relevant_settings_object().global_object());
    auto timelines_to_update = GC::RootVector { m_associated_animation_timelines.values() };
    for (auto const& timeline : timelines_to_update)
        timeline->update_current_time(timestamp);

    // OPTIMIZATION: Events which occurred while there were no listeners were intentionally not sampled. Establish
    //               the current phase as the baseline so a newly added listener only observes future events.
    for (auto& effect : effects_to_synchronize) {
        effect->set_previous_phase(effect->phase());
        effect->set_previous_current_iteration(effect->current_iteration().value_or(0.0));
        effect->clear_per_frame_animation_tick_was_skipped();
    }
}

// https://www.w3.org/TR/web-animations-1/#update-animations-and-send-events
void Document::update_animations_and_send_events(double timestamp)
{
    m_last_animation_frame_timestamp = timestamp;
    auto timelines_to_update = GC::RootVector { m_associated_animation_timelines.values() };

    {
        HTML::TemporaryExecutionContext temporary_execution_context { relevant_settings_object() };
        // 1. Update the current time of all timelines associated with doc passing now as the timestamp.
        for (auto const& timeline : timelines_to_update)
            timeline->update_current_time(timestamp);

        // NB: We dispatch events for all animations regardless of whether they have a timeline
        GC::RootVector<GC::Ref<Animations::Animation>> animations;
        for (auto& animation : m_associated_animations)
            animations.append(animation);

        for (auto& animation : animations) {
            dispatch_events_for_animation_if_necessary(animation);
            if (animation->css_cancellation_disassociation_pending())
                animation->disassociate_from_target_after_css_cancellation();
        }

        // 2. Remove replaced animations for doc.
        remove_replaced_animations();

        // 3. Perform a microtask checkpoint.
        // NB: This is executed by the destructor of the TemporaryExecutionContext above.
    }

    // 4. Let events to dispatch be a copy of doc’s pending animation event queue.
    GC::ConservativeVector<PendingAnimationEvent> events_to_dispatch;
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

    // AD-HOC: Nothing else re-requests a rendering update while time-driven animations are running, since
    //         Document::set_needs_animated_style_update() only requests a frame when its flag flips from
    //         false to true, and the flag is both set and cleared within the same rendering update.
    //         Without this, animations only advance when unrelated tasks happen to schedule rendering
    //         updates. Keep the frame pump going as long as some animation attached to a monotonically
    //         increasing timeline needs main-thread painting or observable animation events.
    for (auto const& animation : m_associated_animations) {
        if (animation.play_state() != Bindings::AnimationPlayState::Running)
            continue;
        auto timeline = animation.timeline();
        if (!timeline || !timeline->is_monotonically_increasing())
            continue;
        if (animation.effect() && is<Animations::KeyframeEffect>(*animation.effect())) {
            auto& effect = static_cast<Animations::KeyframeEffect&>(*animation.effect());
            if (effect.can_skip_per_frame_animation_tick()) {
                effect.note_per_frame_animation_tick_was_skipped();
                continue;
            }
            effect.clear_per_frame_animation_tick_was_skipped();
        }
        ++m_style_invalidation_counters.animation_frame_pump_requests;
        page().client().request_frame();
        break;
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
        for (auto& animation : timeline->associated_animations()) {
            if (!animation.effect() || !animation.effect()->target() || &animation.effect()->target()->document() != this)
                continue;

            if (!animation.is_replaceable())
                continue;

            if (animation.replace_state() != Animations::AnimationReplaceState::Active)
                continue;

            // Composite order is only defined for KeyframeEffects
            if (!animation.effect()->is_keyframe_effect())
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
    HashMap<CSS::PropertyNameAndID, size_t> highest_property_composite_orders;
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
            animation->set_replace_state(Animations::AnimationReplaceState::Removed);

            // - Create an AnimationPlaybackEvent, removeEvent.
            // - Set removeEvent’s type attribute to remove.
            // - Set removeEvent’s currentTime attribute to the current time of animation.
            // - Set removeEvent’s timelineTime attribute to the current time of the timeline with which animation is
            //   associated.
            auto remove_event = Animations::AnimationPlaybackEvent::create(
                HTML::EventNames::remove,
                animation->current_time().has_value() ? Animations::NullableCSSNumberish { animation->current_time()->as_css_numberish() } : Animations::NullableCSSNumberish { Empty {} },
                animation->timeline()->current_time().has_value() ? Animations::NullableCSSNumberish { animation->timeline()->current_time()->as_css_numberish() } : Animations::NullableCSSNumberish { Empty {} },
                HighResolutionTime::current_high_resolution_time(relevant_global_object(*this)));

            // - If animation has a document for timing, then append removeEvent to its document for timing's pending
            //   animation event queue along with its target, animation. For the scheduled event time, use the result of
            //   applying the procedure to convert timeline time to origin-relative time to the current time of the
            //   timeline with which animation is associated.
            if (auto document = animation->document_for_timing()) {
                PendingAnimationEvent pending_animation_event {
                    .event = remove_event,
                    .animation = animation,
                    .target = animation,
                    .scheduled_event_time = animation->timeline()->convert_a_timeline_time_to_an_origin_relative_time(animation->timeline()->current_time()),
                };
                document->append_pending_animation_event(pending_animation_event);
            }
            //   Otherwise, queue a task to dispatch removeEvent at animation. The task source for this task is the DOM
            //   manipulation task source.
            else {
                HTML::queue_global_task(HTML::Task::Source::DOMManipulation, relevant_settings_object().realm().global_object(), GC::create_function(GC::Heap::the(), [animation, remove_event]() {
                    animation->dispatch_event(remove_event);
                }));
            }
        }
    }
}

WebIDL::ExceptionOr<Vector<GC::Ref<Animations::Animation>>> Document::get_animations()
{
    update_style();
    return calculate_get_animations(*this);
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
        if (el.ptr() == &element)
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

void Document::element_id_changed(Badge<DOM::Element>, GC::Ref<DOM::Element> element, Optional<Utf16FlyString> old_id)
{
    for (auto* form_associated_element : m_form_associated_elements_with_form_attribute)
        form_associated_element->element_id_changed({});

    if (element->id().has_value())
        insert_in_tree_order(m_potentially_named_elements, element);
    else if (!element->name().has_value())
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

GC::Ptr<Element> Document::element_by_anchor_name(Utf16FlyString const& name, Node const& querying_node, Function<bool(Element&)> const& is_acceptable) const
{
    // https://drafts.csswg.org/css-shadow-1/#tree-scoped-name
    // If a tree-scoped name is global (such as @font-face names), then when a tree-scoped reference is dereferenced to
    // find it, first search only the tree-scoped names associated with the same root as the tree-scoped reference. If
    // no relevant tree-scoped name is found, and the root is a shadow root, then repeat this search in the root's
    // host's node tree (recursively).
    auto const* node = &querying_node;
    while (auto const* shadow_root = as_if<ShadowRoot>(node->root())) {
        if (auto element = shadow_root->anchor_name_map().last_element_by_name_matching(name, is_acceptable))
            return element;
        node = shadow_root->host();
        if (!node)
            return {};
    }
    return m_anchor_name_map.last_element_by_name_matching(name, is_acceptable);
}

HTML::RadioButtonGroupRegistry& Document::ensure_radio_button_group_registry()
{
    if (!m_radio_button_group_registry)
        m_radio_button_group_registry = heap().allocate<HTML::RadioButtonGroupRegistry>();
    return *m_radio_button_group_registry;
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
    if (m_design_mode_enabled == design_mode_enabled)
        return;

    struct PreviousReadWriteState {
        GC::Ptr<Element> element;
        bool value;
    };
    GC::ConservativeVector<PreviousReadWriteState> previous_read_write_states;
    for_each_in_inclusive_subtree_of_type<Element>([&](Element& element) {
        previous_read_write_states.append({ element, SelectorMatching::element_matches_state(element, CSS::PseudoClass::ReadWrite) });
        return TraversalDecision::Continue;
    });

    m_design_mode_enabled = design_mode_enabled;
    recompute_editable_subtree_flags_and_repaint();
    for (auto const& state : previous_read_write_states)
        CSS::Invalidation::invalidate_style_after_read_write_state_change(*state.element, state.value);
}

// https://html.spec.whatwg.org/multipage/interaction.html#making-entire-documents-editable:-the-designmode-idl-attribute
Utf16FlyString Document::design_mode() const
{
    // The designMode getter steps are to return "on" if this's design mode enabled is true; otherwise "off".
    return design_mode_enabled_state() ? "on"_utf16_fly_string : "off"_utf16_fly_string;
}

WebIDL::ExceptionOr<void> Document::set_design_mode(Utf16View design_mode)
{
    // 1. Let value be the given value, converted to ASCII lowercase.

    // 2. If value is "on" and this's design mode enabled is false, then:
    if (design_mode.equals_ignoring_ascii_case(u"on"sv) && !m_design_mode_enabled) {
        // 1. Set this's design mode enabled to true.
        set_design_mode_enabled_state(true);
        // 2. Reset this's active range's start and end boundary points to be at the start of this.
        if (auto selection = get_selection()) {
            TRY(selection->collapse(this, 0));
            update_layout(UpdateLayoutReason::DocumentSetDesignMode);
        }
        // 3. Run the focusing steps for this's document element, if non-null.
        if (auto* document_element = this->document_element(); document_element)
            HTML::run_focusing_steps(document_element);
    }
    // 3. If value is "off", then set this's design mode enabled to false.
    else if (design_mode.equals_ignoring_ascii_case(u"off"sv)) {
        set_design_mode_enabled_state(false);
    }
    return {};
}

static Element* retarget_from_ua_internal_shadow_root(Element& element)
{
    auto* result = &element;
    while (auto shadow_root = result->containing_shadow_root()) {
        if (!shadow_root->is_user_agent_internal())
            break;
        result = shadow_root->host();
    }
    return result;
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
    GC::Ptr<Element> hit_element;
    (void)hit_test_all(position, [&](Painting::HitTestResult result) {
        if (auto* element = as_if<Element>(result.dom_node())) {
            hit_element = element;
            return TraversalDecision::Break;
        }
        return TraversalDecision::Continue;
    });
    if (hit_element) {
        // AD-HOC: If element is inside a UA internal shadow root, retarget to the host.
        return retarget_from_ua_internal_shadow_root(*hit_element);
    }

    // 3. If the document has a root element, return the root element and terminate these steps.
    if (auto const* root_element = document_element())
        return root_element;

    // 4. Return null.
    return nullptr;
}

// https://drafts.csswg.org/cssom-view/#dom-document-elementsfrompoint
GC::RootVector<GC::Ref<Element>> Document::elements_from_point(double x, double y)
{
    // 1. Let sequence be a new empty sequence.
    GC::RootVector<GC::Ref<Element>> sequence;

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
    (void)hit_test_all(position, [&](Painting::HitTestResult result) {
        if (auto* element = as_if<Element>(result.dom_node())) {
            // AD-HOC: If element is inside a UA internal shadow root, retarget to the host.
            element = retarget_from_ua_internal_shadow_root(*element);
            // AD-HOC: Avoid adding duplicates when multiple boxes resolve to the same element, or when multiple
            // internal elements retarget to the same host.
            if (!sequence.contains_slow(GC::Ref { *element }))
                sequence.append(*element);
        }
        return TraversalDecision::Continue;
    });

    // 4. If the document has a root element, and the last item in sequence is not the root element,
    //    append the root element to sequence.
    if (auto* root_element = document_element(); root_element && (sequence.is_empty() || (sequence.last().ptr() != root_element)))
        sequence.append(*root_element);

    // 5. Return sequence.
    return sequence;
}

static bool shadow_root_is_allowed_for_caret_position(ShadowRoot const& shadow_root, Document::CaretPositionFromPointOptions const& options)
{
    for (auto const& allowed_shadow_root : options.shadow_roots) {
        if (shadow_root.is_shadow_including_inclusive_ancestor_of(allowed_shadow_root))
            return true;
    }
    return false;
}

// https://drafts.csswg.org/cssom-view/#dom-document-caretpositionfrompoint
GC::Ptr<CaretPosition> Document::caret_position_from_point(double x, double y, CaretPositionFromPointOptions const& options)
{
    // 1. If there is no viewport associated with the document, return null.
    // 2. If either argument is negative, x is greater than the viewport width excluding the size of a rendered scroll
    //    bar (if any), or y is greater than the viewport height excluding the size of a rendered scroll bar (if any),
    //    return null.
    auto viewport_rect = this->viewport_rect();
    CSSPixelPoint position { x, y };
    // FIXME: This should account for the size of the scroll bar.
    if (x < 0 || y < 0 || position.x() > viewport_rect.width() || position.y() > viewport_rect.height())
        return nullptr;

    // Ensure the layout tree exists prior to hit testing.
    update_layout(UpdateLayoutReason::DocumentCaretPositionFromPoint);

    // 3. If at the coordinates x,y in the viewport no text insertion point indicator would have been inserted when
    //    applying the transforms that apply to the descendants of the viewport, return null.
    auto caret_position = caret_position_from_point(position);
    if (!caret_position.has_value())
        return nullptr;

    // FIXME: 4. If at the coordinates x,y in the viewport a text insertion point indicator would have been inserted
    //           in a text entry widget which is also a replaced element, when applying the transforms that apply to
    //           the descendants of the viewport, return a caret position for the text entry widget.

    // 5. Otherwise, retarget shadow tree positions whose roots are not allowed by options.shadowRoots.
    auto start_node = caret_position->boundary.node;
    auto start_offset = caret_position->boundary.offset;
    auto* shadow_root = as_if<ShadowRoot>(start_node->root());
    while (shadow_root && !shadow_root_is_allowed_for_caret_position(*shadow_root, options)) {
        auto* host = shadow_root->host();
        auto* host_parent = host->parent();
        if (!host_parent)
            return nullptr;
        start_offset = host->index();
        start_node = *host_parent;
        shadow_root = as_if<ShadowRoot>(start_node->root());
    }

    return CaretPosition::create(start_node, start_offset, caret_position->debug_rect.map([](auto const& rect) {
        return rect.template to_type<float>();
    }));
}

// https://drafts.csswg.org/cssom-view/#dom-document-scrollingelement
GC::Ptr<Element const> Document::scrolling_element() const
{
    // 1. If the Document is in quirks mode, follow these substeps:
    if (in_quirks_mode()) {
        // 1. If the body element exists, and it is not potentially scrollable, return the body element and abort these steps.
        //    For this purpose, a value of overflow:clip on the the body element’s parent element must be treated as overflow:hidden.
        if (auto const* body_element = body(); body_element && !body_element->is_potentially_scrollable(Element::TreatOverflowClipOnBodyParentAsOverflowHidden::Yes))
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

Vector<Utf16FlyString> Document::supported_property_names() const
{
    // The supported property names of a Document object document at any moment consist of the following,
    // in tree order according to the element that contributed them, ignoring later duplicates,
    // and with values from id attributes coming before values from name attributes when the same element contributes both:
    OrderedHashTable<Utf16FlyString> names;

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

    Vector<Utf16FlyString> result;
    result.ensure_capacity(names.size());
    for (auto const& name : names)
        result.append(name);
    return result;
}

bool Document::is_named_element_with_name(Element const& element, Utf16FlyString const& name)
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

Vector<GC::Ref<DOM::Element>> Document::named_elements_with_name(Utf16FlyString const& name) const
{
    Vector<GC::Ref<DOM::Element>> named_elements;

    for (auto const& element : potentially_named_elements()) {
        if (is_named_element_with_name(element, name))
            named_elements.append(element);
    }

    return named_elements;
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
    GC::RootVector<GC::Ref<ResizeObserver::ResizeObserver>> resize_observers;
    for (auto const& observer : m_resize_observers) {
        if (observer)
            resize_observers.append(*observer);
    }

    for (auto const& observer : resize_observers) {
        observer->remove_dead_observations();

        // 1. Clear observer’s [[activeTargets]], and [[skippedTargets]].
        observer->active_targets().clear();
        observer->skipped_targets().clear();

        // 2. For each observation in observer.[[observationTargets]] run this step:
        for (auto& observation : observer->observation_targets()) {
            // 1. If observation.isActive() is true
            if (observation->is_active()) {
                auto target = observation->target();
                VERIFY(target);

                // 1. Let targetDepth be result of calculate depth for node for observation.target.
                auto target_depth = calculate_depth_for_node(*target);

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
    GC::RootVector<GC::Ref<ResizeObserver::ResizeObserver>> resize_observers;
    for (auto const& observer : m_resize_observers) {
        if (observer)
            resize_observers.append(*observer);
    }

    // Keep all gathered targets alive while resize observer callbacks run.
    GC::RootVector<GC::Ref<Element>> active_targets;
    for (auto const& observer : resize_observers) {
        for (auto const& observation : observer->active_targets()) {
            if (auto target = observation->target())
                active_targets.append(*target);
        }
    }

    for (auto const& observer : resize_observers) {
        // 1. If observer.[[activeTargets]] slot is empty, continue.
        if (observer->active_targets().is_empty()) {
            continue;
        }

        // 2. Let entries be an empty list of ResizeObserverEntryies.
        GC::RootVector<GC::Ref<ResizeObserver::ResizeObserverEntry>> entries;

        // 3. For each observation in [[activeTargets]] perform these steps:
        for (auto const& observation : observer->active_targets()) {
            auto target = observation->target();
            if (!target)
                continue;

            // 1. Let entry be the result of running create and populate a ResizeObserverEntry given observation.target.
            auto entry = ResizeObserver::ResizeObserverEntry::create_and_populate(*target).release_value_but_fixme_should_propagate_errors();

            // 2. Add entry to entries.
            entries.append(entry);

            // 3. Set observation.lastReportedSizes to matching entry sizes.
            switch (observation->observed_box()) {
            case ResizeObserver::ObservedBox::BorderBox:
                // Matching sizes are entry.borderBoxSize if observation.observedBox is "border-box"
                observation->set_last_reported_sizes(entry->border_box_size());
                break;
            case ResizeObserver::ObservedBox::ContentBox:
                // Matching sizes are entry.contentBoxSize if observation.observedBox is "content-box"
                observation->set_last_reported_sizes(entry->content_box_size());
                break;
            case ResizeObserver::ObservedBox::DevicePixelContentBox:
                // Matching sizes are entry.devicePixelContentBoxSize if observation.observedBox is "device-pixel-content-box"
                observation->set_last_reported_sizes(entry->device_pixel_content_box_size());
                break;
            }

            // 4. Set targetDepth to the result of calculate depth for node for observation.target.
            auto target_depth = calculate_depth_for_node(*target);

            // 5. Set shallowestTargetDepth to targetDepth if targetDepth < shallowestTargetDepth
            if (target_depth < shallowest_target_depth)
                shallowest_target_depth = target_depth;
        }

        if (entries.is_empty()) {
            observer->active_targets().clear();
            continue;
        }

        // 4. Invoke observer.[[callback]] with entries.
        ResizeObserver::invoke_resize_observer_callback(*observer, entries);

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
        if (!observer)
            continue;
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
        if (!observer)
            continue;
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

void Document::for_each_active_css_style_sheet(Function<void(CSS::CSSStyleSheet&)> const& callback) const
{
    if (m_style_sheets) {
        for (auto& style_sheet : m_style_sheets->sheets()) {
            if (!style_sheet->disabled())
                callback(*style_sheet);
        }
    }

    if (m_adopted_style_sheets) {
        for_each_adopted_style_sheet(*m_adopted_style_sheets, [&](auto& style_sheet) {
            if (!style_sheet.disabled())
                callback(style_sheet);
        });
    }

    if (m_dynamic_view_transition_style_sheet) {
        callback(*m_dynamic_view_transition_style_sheet);
    }
}

double Document::ensure_element_shared_css_random_base_value(CSS::RandomCachingKey const& random_caching_key)
{
    return m_element_shared_css_random_base_value_cache.ensure(random_caching_key, []() {
        static XorShift128PlusRNG random_number_generator;
        return random_number_generator.get();
    });
}

static Optional<CSS::CSSStyleSheet&> find_style_sheet_with_url(Utf16View url, CSS::CSSStyleSheet& style_sheet)
{
    if (style_sheet.href_for_bindings() == url)
        return style_sheet;

    for (auto& import_rule : style_sheet.import_rules()) {
        if (import_rule->loaded_style_sheet()) {
            if (auto match = find_style_sheet_with_url(url, *import_rule->loaded_style_sheet()); match.has_value())
                return match;
        }
    }

    return {};
}

Optional<Utf16String> Document::get_style_sheet_source(CSS::StyleSheetIdentifier const& identifier) const
{
    switch (identifier.type) {
    case CSS::StyleSheetIdentifier::Type::StyleElement:
        if (identifier.dom_element_unique_id.has_value()) {
            if (auto* node = Node::from_unique_id(*identifier.dom_element_unique_id)) {
                if (node->is_html_style_element()) {
                    if (auto* sheet = as<HTML::HTMLStyleElement>(*node).sheet())
                        return sheet->source_text();
                }
                if (node->is_svg_style_element()) {
                    if (auto* sheet = as<SVG::SVGStyleElement>(*node).sheet())
                        return sheet->source_text();
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
                    return match->source_text();
            }
        }

        if (m_adopted_style_sheets) {
            Optional<Utf16String> result;
            for_each_adopted_style_sheet(*m_adopted_style_sheets, [&](auto& style_sheet) {
                if (result.has_value())
                    return;

                if (auto match = find_style_sheet_with_url(identifier.url.value(), style_sheet); match.has_value())
                    result = match->source_text();
            });
            return result;
        }

        return {};
    }
    case CSS::StyleSheetIdentifier::Type::UserAgent:
        return CSS::StyleComputer::user_agent_style_sheet_source(identifier.url->utf16_view());
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
    m_shadow_roots.remove(shadow_root);
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
    m_elements_with_pending_top_layer_membership_change.append(element);
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
    m_elements_with_pending_top_layer_membership_change.append(element);

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

    m_elements_with_pending_top_layer_membership_change.append(element);
}

// https://drafts.csswg.org/css-position-4/#process-top-layer-removals
void Document::process_top_layer_removals()
{
    // 1. For each element el in doc’s pending top layer removals: if el’s computed value of overlay is none, or el is
    //    not rendered, remove el from doc’s top layer and pending top layer removals.
    GC::RootVector<GC::Ref<Element>> elements_to_remove;
    // NB: Called during top layer processing.
    for (auto& element : m_top_layer_pending_removals) {
        // FIXME: Implement overlay property
        auto const* layout_node = element->unsafe_layout_node();
        if (!layout_node || !Painting::has_committed_box(*layout_node)) {
            elements_to_remove.append(element);
        }
    }

    for (auto& element : elements_to_remove) {
        m_top_layer_elements.remove(element);
        m_top_layer_pending_removals.remove(element);
    }
}

// The top layer is treated as a single rebuild zone: any membership change detaches the box
// subtree of every rendered member and re-marks the members, after which the top layer pass of
// the tree builder recreates all their boxes in top layer order. Runs after style update so
// that elements that left the top layer are classified by their up-to-date computed display.
void Document::process_pending_top_layer_layout_changes()
{
    if (m_elements_with_pending_top_layer_membership_change.is_empty() && !m_top_layer_needs_layout_zone_rebuild)
        return;

    auto elements_with_membership_change = move(m_elements_with_pending_top_layer_membership_change);
    m_top_layer_needs_layout_zone_rebuild = false;

    // An already pending full build recreates every box anyway.
    if (!m_layout_root || needs_full_layout_tree_update())
        return;

    // Marks are applied only after every detach has run: detaching clears the flags across the
    // detached subtree and would wipe the fresh mark of a member nested inside another member.
    GC::RootVector<GC::Ref<Element>> elements_to_mark_for_layout_tree_update;

    for (auto const& element : elements_with_membership_change) {
        // NB: Elements that changed membership more than once are processed by their final state.
        if (element->rendered_in_top_layer()) {
            // An entering element whose box is still attached at its normal position leaves
            // anonymous wrappers and inline fragments behind; the parent subtree rebuild heals
            // that structure, while the top layer pass rebuilds the element itself.
            // NB: Called during top layer processing, outside layout tree construction.
            auto* element_layout_node = element->unsafe_layout_node();
            bool element_has_box_at_normal_position = element_layout_node && element_layout_node->parent() && !element_layout_node->topmost_layout_node_of_top_layer_placement();
            if (element_has_box_at_normal_position) {
                if (auto* flat_tree_parent = element->flat_tree_parent(); flat_tree_parent && !flat_tree_parent->is_document())
                    flat_tree_parent->set_needs_layout_tree_update(true, SetNeedsLayoutTreeUpdateReason::TopLayerMembershipChange);
            }
        } else {
            // A leaving element that is still rendered (fullscreen exit, mostly) needs a box
            // back among already-built sibling boxes, which only a full rebuild can order.
            auto const* box_values = element->style_group<CSS::ComputedValues::BoxValues>();
            bool element_is_still_rendered = element->is_connected()
                && box_values
                && !CSS::display_from_ffi_display(box_values->display).is_none();
            if (element_is_still_rendered) {
                invalidate_layout_tree(InvalidateLayoutTreeReason::TopLayerElementStillRenderedAfterRemoval);
                return;
            }
            Layout::detach_top_layer_element_layout_subtree(element);
            if (element->is_connected())
                elements_to_mark_for_layout_tree_update.append(element);
        }
    }

    for (auto const& member : m_top_layer_elements) {
        if (!member->rendered_in_top_layer())
            continue;
        Layout::detach_top_layer_element_layout_subtree(member);
        elements_to_mark_for_layout_tree_update.append(member);
    }

    for (auto const& element : elements_to_mark_for_layout_tree_update)
        element->set_needs_layout_tree_update(true, SetNeedsLayoutTreeUpdateReason::TopLayerMembershipChange);
}

// https://html.spec.whatwg.org/multipage/popover.html#topmost-auto-popover
GC::Ptr<HTML::HTMLElement> Document::topmost_auto_or_hint_popover()
{
    // To find the topmost auto or hint popover given a Document document, perform the following steps. They return an HTML element or null.

    // 1. If document's showing hint popover list is not empty, then return document's showing hint popover list's last element.
    if (!m_showing_hint_popover_list.is_empty())
        return m_showing_hint_popover_list.last();

    // 2. If document's showing auto popover list is not empty, then return document's showing auto popover list's last element.
    if (!m_showing_auto_popover_list.is_empty())
        return m_showing_auto_popover_list.last();

    // 3. Return null.
    return {};
}

void Document::set_needs_to_refresh_scroll_state(bool b)
{
    // NB: Propagating scroll state invalidation.
    if (has_committed_viewport_box())
        paint_state().set_needs_to_refresh_scroll_state(*this, b);
}

Vector<GC::Root<Range>> Document::find_matching_text(Utf16View query, CaseSensitivity case_sensitivity)
{
    // Ensure the layout tree exists before searching for text matches.
    update_layout(UpdateLayoutReason::DocumentFindMatchingText);

    if (!layout_node())
        return {};

    auto const& text_blocks = layout_node()->text_blocks();
    if (text_blocks.is_empty())
        return {};

    Vector<GC::Root<Range>> matches;
    for (auto const& text_block : text_blocks) {
        size_t offset = 0;
        size_t i = 0;
        Utf16View text_view { text_block.text };
        auto* match_start_position = text_block.positions.data();
        while (true) {
            auto match_index = case_sensitivity == CaseSensitivity::CaseInsensitive
                ? text_view.find_code_unit_offset_ignoring_case(query, offset)
                : text_view.find_code_unit_offset(query, offset);
            if (!match_index.has_value())
                break;

            for (; i < text_block.positions.size() - 1 && match_index.value() > text_block.positions[i + 1].start_offset; ++i)
                match_start_position = &text_block.positions[i + 1];

            auto start_position = match_index.value() - match_start_position->start_offset + match_start_position->dom_offset_within_node;
            auto start_dom_node = match_start_position->dom_node.ptr();
            VERIFY(start_dom_node);

            auto* match_end_position = match_start_position;
            for (; i < text_block.positions.size() - 1 && (match_index.value() + query.length_in_code_units() > text_block.positions[i + 1].start_offset); ++i)
                match_end_position = &text_block.positions[i + 1];

            auto end_dom_node = match_end_position->dom_node.ptr();
            VERIFY(end_dom_node);
            auto end_position = match_index.value() + query.length_in_code_units() - match_end_position->start_offset + match_end_position->dom_offset_within_node;

            if (&start_dom_node->root() != &end_dom_node->root()
                || !start_dom_node->is_connected()
                || !end_dom_node->is_connected()
                || start_position > start_dom_node->length()
                || end_position > end_dom_node->length()) {
                offset = match_index.value() + query.length_in_code_units() + 1;
                if (offset >= text_view.length_in_code_units())
                    break;
                continue;
            }

            matches.append(Range::create(*start_dom_node, start_position, *end_dom_node, end_position));
            match_start_position = match_end_position;
            offset = match_index.value() + query.length_in_code_units() + 1;
            if (offset >= text_view.length_in_code_units())
                break;
        }
    }

    return matches;
}

// https://dom.spec.whatwg.org/#document-allow-declarative-shadow-roots
HTML::HTMLParser::AllowDeclarativeShadowRoots Document::allow_declarative_shadow_roots() const
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

    // AD-HOC: Consider pending CSS @import rules as render-blocking
    if (!m_pending_css_import_rules.is_empty())
        return true;

    return !m_render_blocking_elements.is_empty() || allows_adding_render_blocking_elements();
}

// https://html.spec.whatwg.org/multipage/dom.html#allows-adding-render-blocking-elements
bool Document::allows_adding_render_blocking_elements() const
{
    // A document allows adding render-blocking elements if document's content type is "text/html" and the body element of document is null.
    return content_type() == u"text/html"sv && !body();
}

void Document::add_render_blocking_element(GC::Ref<Element> element)
{
    m_render_blocking_elements.set(element);
}

void Document::remove_render_blocking_element(GC::Ref<Element> element)
{
    m_render_blocking_elements.remove(element);
    if (!m_render_blocking_elements.is_empty())
        return;

    if (auto navigable = this->navigable()) {
        if (auto container = navigable->container())
            container->set_needs_repaint(InvalidateDisplayList::Yes);
    }

    page().client().request_frame();
}

// https://fullscreen.spec.whatwg.org/#run-the-fullscreen-steps
void Document::run_fullscreen_steps()
{
    // 1. Let pendingEvents be document’s list of pending fullscreen events.
    GC::ConservativeVector<PendingFullscreenEvent> pending_events;
    pending_events.extend(m_pending_fullscreen_events);

    // 2. Empty document’s list of pending fullscreen events.
    m_pending_fullscreen_events.clear();

    // 3. For each (type, element) in pendingEvents:
    for (auto const& [type, element, request_type] : pending_events) {
        // 1. Let target be element if element is connected and its node document is document, and otherwise let target be document.
        GC::Ref<Node> target { *this };
        if (element->is_connected() && &element->document() == this)
            target = element;

        // 2. Fire an event named type, with its bubbles and composed attributes set to true, at target.
        switch (type) {
        case PendingFullscreenEvent::Type::Change:
            target->dispatch_event(Event::create_bubbling_composed(
                request_type == Fullscreen::RequestType::WebKit ? HTML::EventNames::webkitfullscreenchange : HTML::EventNames::fullscreenchange,
                HighResolutionTime::current_high_resolution_time(relevant_global_object(*this))));
            break;
        case PendingFullscreenEvent::Type::Error:
            target->dispatch_event(Event::create_bubbling_composed(
                request_type == Fullscreen::RequestType::WebKit ? HTML::EventNames::webkitfullscreenerror : HTML::EventNames::fullscreenerror,
                HighResolutionTime::current_high_resolution_time(relevant_global_object(*this))));
            break;
        }
    }
}

void Document::append_pending_fullscreen_change(PendingFullscreenEvent::Type type, GC::Ref<Element> element, Fullscreen::RequestType request_type)
{
    m_pending_fullscreen_events.append(PendingFullscreenEvent { type, element, request_type });
    page().client().request_frame();
}

// https://fullscreen.spec.whatwg.org/#fullscreen-an-element
void Document::fullscreen_element_within_doc(GC::Ref<Element> element, Fullscreen::RequestType request_type)
{
    // FIXME: Spec issue:  Finding topmost popover ancestor algorithm takes different parameters than those described
    //        by the fullscreen spec. Since the new algorithm takes 4 parameters, with the new "popover list", we must
    //        also account for the auto popover list.
    //        See: https://github.com/whatwg/fullscreen/issues/245
    auto const get_hide_until = [&](auto const& popover_list) {
        return HTML::HTMLElement::topmost_popover_ancestor(element, popover_list, nullptr, HTML::IsPopover::No);
    };

    // 1. Let hideUntil be the result of running topmost popover ancestor given element, null, and false.
    auto hide_until = get_hide_until(showing_hint_popover_list());

    if (hide_until == nullptr)
        hide_until = get_hide_until(showing_auto_popover_list());

    Variant<GC::Ptr<HTML::HTMLElement>, GC::Ptr<Document>> hide_until_argument { hide_until };

    // 2. If hideUntil is null, then set hideUntil to element’s node document.
    if (hide_until == nullptr)
        hide_until_argument = GC::Ptr { element->document() };

    // 3. Run hide all popovers until given hideUntil, false, and true.
    HTML::HTMLElement::hide_all_popovers_until(hide_until_argument, HTML::FocusPreviousElement::No, HTML::FireEvents::Yes);

    // 4. Set element’s fullscreen flag.
    element->set_fullscreen_flag(true);
    element->set_fullscreen_request_type(request_type);

    // 5. Remove from the top layer immediately given element.
    remove_an_element_from_the_top_layer_immediately(element);

    // 6. Add to the top layer given element.
    add_an_element_to_the_top_layer(element);
}

// https://fullscreen.spec.whatwg.org/#fullscreen-element
GC::Ptr<Element> Document::fullscreen_element() const
{
    // All documents have an associated fullscreen element. The fullscreen element is the topmost element in the
    // document’s top layer whose fullscreen flag is set, if any, and null otherwise.
    for (auto const& el : top_layer_elements().in_reverse()) {
        if (el->is_fullscreen_element())
            return el;
    }
    return nullptr;
}

// https://fullscreen.spec.whatwg.org/#dom-document-fullscreenelement
GC::Ptr<Element> Document::retargeted_fullscreen_element() const
{
    auto fullscreen_element = this->fullscreen_element();
    if (!fullscreen_element)
        return nullptr;

    // 1. If this is a shadow root and its host is not connected, then return null.
    // NB: We're not a shadow root. See ShadowRoot::retargeted_fullscreen_element().

    // 2. Let candidate be the result of retargeting fullscreen element against this.
    auto* candidate = retarget(fullscreen_element.ptr(), const_cast<Document*>(this));
    if (!candidate)
        return nullptr;

    // 3. If candidate and this are in the same tree, then return candidate.
    if (auto* retargeted_element = as<Element>(candidate); retargeted_element && &retargeted_element->root() == &root())
        return retargeted_element;

    // 4. Return null.
    return nullptr;
}

// https://fullscreen.spec.whatwg.org/#dom-document-fullscreen
bool Document::fullscreen() const
{
    // The fullscreen getter steps are to return false if this's fullscreen element is null, and true otherwise.
    return fullscreen_element() != nullptr;
}

// https://fullscreen.spec.whatwg.org/#dom-document-fullscreenenabled
bool Document::fullscreen_enabled() const
{
    // FIXME: Implement check policy check and "is supported" check.
    return is_allowed_to_use_feature(PolicyControlledFeature::Fullscreen);
}

// https://fullscreen.spec.whatwg.org/#fully-exit-fullscreen
void Document::fully_exit_fullscreen()
{
    // 1. If document’s fullscreen element is null, terminate these steps.
    GC::Ptr<Element> fullscreened_element = fullscreen_element();
    if (!fullscreened_element)
        return;

    // 2. Unfullscreen elements whose fullscreen flag is set, within document’s top layer, except for document’s fullscreen element.
    GC::RootVector<GC::Ref<Element>, 8> fullscreen_elements;
    for (auto const& element : top_layer_elements()) {
        if (element->is_fullscreen_element() && element != fullscreened_element)
            fullscreen_elements.append(element);
    }

    for (auto const& element : fullscreen_elements)
        unfullscreen_element(element);

    // 3. Exit fullscreen document.
    HTML::TemporaryExecutionContext context { relevant_settings_object(), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
    exit_fullscreen(nullptr);
}

// https://fullscreen.spec.whatwg.org/#exit-fullscreen
void Document::exit_fullscreen(GC::Ptr<WebIDL::Promise> promise)
{
    // 2. If doc is not fully active or doc’s fullscreen element is null, then reject promise with a TypeError exception
    //    and return promise.
    if (!is_fully_active() || !fullscreen_element()) {
        if (promise) {
            auto& realm = WebIDL::promise_realm(*promise);
            WebIDL::reject_promise(*promise, JS::TypeError::create(realm, "Document not fully active or no fullscreen element."sv));
        }
        return;
    }

    // 3. Let resize be false.
    bool resize = false;

    // 4. Let docs be the result of collecting documents to unfullscreen given doc.
    auto docs = collect_documents_to_unfullscreen();

    // 5. Let topLevelDoc be doc’s node navigable’s top-level traversable’s active document.
    auto top_level_doc = as<HTML::LocalTraversableNavigable>(*navigable()->top_level_traversable()).active_document();

    // 6. If topLevelDoc is in docs, and it is a simple fullscreen document, then set doc to topLevelDoc and resize to true.
    GC::Ref<Document> doc { *this };
    if (top_level_doc && top_level_doc->is_simple_fullscreen_document() && docs->elements().contains_slow(GC::Ref { *top_level_doc })) {
        doc = *top_level_doc;
        resize = true;
    }

    // 7. If doc’s fullscreen element is not connected:
    if (auto fullscreen_element = doc->fullscreen_element(); !fullscreen_element->is_connected()) {
        // 1. Append (fullscreenchange, doc’s fullscreen element) to doc’s list of pending fullscreen events.
        doc->append_pending_fullscreen_change(PendingFullscreenEvent::Type::Change, *fullscreen_element, fullscreen_element->fullscreen_request_type());

        // 2. Unfullscreen doc’s fullscreen element.
        doc->unfullscreen_element(*fullscreen_element);
    }

    // 8. Return promise, and run the remaining steps in parallel.
    page().enqueue_fullscreen_exit(doc, resize, promise);
}

void Document::webkit_exit_fullscreen()
{
    exit_fullscreen(nullptr);
}

// https://fullscreen.spec.whatwg.org/#unfullscreen-a-document
void Document::unfullscreen()
{
    // To unfullscreen a document, unfullscreen all elements, within document’s top layer, whose fullscreen flag is set.
    // NB: This has to be a copy of the list of those elements, since unfullscreen an element immediately removes it
    //     from the top layer, invalidating iterators.
    auto fullscreen_elements = GC::Heap::the().allocate<GC::HeapVector<GC::Ref<Element>>>();
    for (auto el : top_layer_elements()) {
        if (el->is_fullscreen_element())
            fullscreen_elements->elements().append(el);
    }

    for (auto el : fullscreen_elements->elements())
        unfullscreen_element(el);
}

// https://fullscreen.spec.whatwg.org/#simple-fullscreen-document
bool Document::is_simple_fullscreen_document() const
{
    // A document is said to be a simple fullscreen document if there is exactly one element in its top layer that has its fullscreen flag set.
    u32 total = 0;
    for (auto const& element : top_layer_elements()) {
        if (element->is_fullscreen_element())
            ++total;

        if (total > 1)
            return false;
    }
    return total == 1;
}

// https://fullscreen.spec.whatwg.org/#collect-documents-to-unfullscreen
GC::Ref<GC::HeapVector<GC::Ref<Document>>> Document::collect_documents_to_unfullscreen()
{
    // 1. Let docs be an ordered set consisting of doc.
    auto docs = GC::Heap::the().allocate<GC::HeapVector<GC::Ref<Document>>>();
    docs->elements().append(*this);

    // 2. While true:
    while (true) {
        // 1. Let lastDoc be docs’s last document.
        auto last_doc = docs->elements().last();

        // 2. Assert: lastDoc’s fullscreen element is not null.
        VERIFY(last_doc->fullscreen_element());

        // 3. If lastDoc is not a simple fullscreen document, break.
        if (!last_doc->is_simple_fullscreen_document())
            break;

        // 4. Let container be lastDoc’s node navigable’s container.
        // Note on spec: It doesn't first check if `node navigable` is null.
        auto container = last_doc->navigable() ? last_doc->navigable()->container() : nullptr;

        // 5. If container is null, then break.
        if (!container)
            break;

        // 6. If container’s iframe fullscreen flag is set, break.
        if (auto* iframe_element = as_if<HTML::HTMLIFrameElement>(container.ptr()); iframe_element && iframe_element->iframe_fullscreen_flag())
            break;

        // 7. Append container’s node document to docs.
        docs->elements().append(container->document());
    }

    // 3. Return docs.
    return docs;
}

// https://fullscreen.spec.whatwg.org/#unfullscreen-an-element
void Document::unfullscreen_element(GC::Ref<Element> element)
{
    // To unfullscreen an element, unset element’s fullscreen flag and iframe fullscreen flag (if any), and remove from
    // the top layer immediately given element.
    element->set_fullscreen_flag(false);
    if (auto* iframe_element = as_if<HTML::HTMLIFrameElement>(element.ptr()))
        iframe_element->set_iframe_fullscreen_flag(false);

    remove_an_element_from_the_top_layer_immediately(element);
}

// https://dom.spec.whatwg.org/#document-allow-declarative-shadow-roots
void Document::set_allow_declarative_shadow_roots(HTML::HTMLParser::AllowDeclarativeShadowRoots allow)
{
    m_allow_declarative_shadow_roots = allow;
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#parse-html-from-a-string
void Document::parse_html_from_a_string(Utf16View html)
{
    // 1. Set document's type to "html".
    set_document_type(DOM::Document::Type::HTML);

    // 2. Let parser be a new HTML parser whose allow declarative shadow roots is document's allow declarative shadow roots,
    //    associated with document.
    // 3. Place html into the input stream for parser. The encoding confidence is irrelevant.
    auto scripting_mode = is_scripting_enabled() ? HTML::ParserScriptingMode::Normal : HTML::ParserScriptingMode::Disabled;
    auto parser = HTML::HTMLParser::create_for_decoded_string(*this, html, scripting_mode, "UTF-8"_utf16);
    parser->set_allow_declarative_shadow_roots(allow_declarative_shadow_roots());

    // 4. Start parser and let it run until it has consumed all the characters just inserted into the input stream.
    parser->run(url());
}

InputEventsTarget* Document::active_input_events_target(Node const* for_node)
{
    auto focused_area = this->focused_area();
    if (!focused_area)
        return nullptr;

    if (focused_area->is_editable_or_editing_host()) {
        if (!for_node || m_editing_host_manager->is_within_active_contenteditable(*for_node))
            return m_editing_host_manager.ptr();
    }
    if (auto* form_text_element = as_if<HTML::FormAssociatedTextControlElement>(*focused_area)) {
        if (!for_node || for_node->find_in_shadow_including_ancestry([&](Node const& it) { return &it == focused_area.ptr(); }))
            return form_text_element;
    }
    return nullptr;
}

GC::Ptr<DOM::Position> Document::cursor_position() const
{
    auto const focused_area = this->focused_area();
    if (!focused_area)
        return nullptr;

    Optional<HTML::FormAssociatedTextControlElement const&> target {};
    if (auto const* input_element = as_if<HTML::HTMLInputElement>(*focused_area)) {
        // Some types of <input> tags shouldn't have a cursor, like buttons
        if (!input_element->can_have_text_editing_cursor())
            return nullptr;
        target = *input_element;
    } else if (auto const* text_area_element = as_if<HTML::HTMLTextAreaElement>(*focused_area)) {
        target = *text_area_element;
    }

    if (target.has_value())
        return target->cursor_position();

    if (focused_area->is_editable_or_editing_host())
        return m_selection->cursor_position();

    return nullptr;
}

Optional<CSSPixelRect> Document::current_caret_rect()
{
    // Returns the bounds of the current text caret in viewport-relative CSS pixels. Used to position platform overlays
    // such as the IME candidate window. Returns nothing when no editable element is focused or when layout isn't ready.
    auto position = cursor_position();
    if (!position)
        return {};
    auto& dom_node = *position->node();

    update_layout(UpdateLayoutReason::InputCaretRect);

    auto* layout_node = dom_node.layout_node();
    if (!layout_node)
        return {};

    // The caret rects computed here are document-relative (absolute). Platform IME overlays are positioned relative to
    // the viewport — so translate by scroll offset and map through any containing navigables to the top-level viewport.
    auto to_viewport_rect = [this](CSSPixelRect rect) -> CSSPixelRect {
        auto navigable = this->navigable();
        if (!navigable)
            return rect;
        auto scroll = navigable->viewport_scroll_offset();
        CSSPixelRect viewport_rect { rect.x() - scroll.x(), rect.y() - scroll.y(), rect.width(), rect.height() };
        return navigable->to_top_level_rect(viewport_rect);
    };

    if (auto* text = as_if<DOM::Text>(dom_node)) {
        auto text_slots = Layout::TextOffsetMapping { *text }.slot_ids();
        if (!text_slots.is_empty()) {
            auto result = Layout::RustFFI::layout_arena_text_caret_rect_in_dom_range(
                layout_node->arena_handle(), text_slots.data(), text_slots.size(), position->offset());
            if (result.has_value)
                return to_viewport_rect(result.rect);
        }
    }

    // Empty editable elements have no fragments; fall back to the caret position for the cursor's child offset
    // (which accounts for empty lines rendered by <br>), or the padding-box corner.
    if (auto* node_with_style = as_if<Layout::NodeWithStyle>(*layout_node)) {
        if (Painting::is_paintable_with_lines(*node_with_style))
            return to_viewport_rect(Painting::caret_rect_for_child_offset(*node_with_style, position->offset()));
        if (Painting::has_committed_box(*node_with_style)) {
            auto content_box = Painting::absolute_padding_box_rect(*node_with_style);
            return to_viewport_rect(CSSPixelRect { content_box.x(), content_box.y(), 1, node_with_style->line_height() });
        }
    }
    return {};
}

void Document::reset_cursor_blink_cycle()
{
    m_cursor_blink_cycle_start_time_ns = MonotonicTime::now().nanoseconds();
}

void Document::set_cursor_position_needs_repaint()
{
    auto repaint_position = [](DOM::Position& position) {
        auto node = position.node();
        if (auto* text = as_if<DOM::Text>(*node)) {
            if (auto* layout_text_node = as_if<Layout::TextNode>(text->unsafe_layout_node()))
                layout_text_node->set_needs_repaint();
            return;
        }
        node->set_needs_repaint();
    };

    auto position = cursor_position();

    // NB: Also repaint the node the cursor moved away from, so the old caret disappears.
    if (m_previously_repainted_cursor_position && (!position || m_previously_repainted_cursor_position->node() != position->node()))
        repaint_position(*m_previously_repainted_cursor_position);
    m_previously_repainted_cursor_position = position;

    if (position)
        repaint_position(*position);
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

GC::Ptr<HTML::LocalNavigable> Document::navigable() const
{
    return m_navigable.ptr();
}

void Document::set_navigable(GC::Ptr<HTML::LocalNavigable> navigable)
{
    if (m_navigable == navigable)
        return;

    auto previous_traversable = m_navigable ? m_navigable->traversable_navigable() : nullptr;
    m_navigable = navigable.ptr();
    HTML::main_thread_event_loop().document_navigable_did_change({});

    if (previous_traversable)
        previous_traversable->page().update_needs_beforeunload_check();
    if (navigable) {
        auto new_traversable = navigable->traversable_navigable();
        if (new_traversable && new_traversable != previous_traversable)
            new_traversable->page().update_needs_beforeunload_check();
    }
}

void Document::set_needs_repaint(InvalidateDisplayList should_invalidate_display_list)
{
    auto navigable = this->navigable();

    if (should_invalidate_display_list == InvalidateDisplayList::Yes) {
        set_needs_to_record_display_list();
    }

    if (!navigable)
        return;

    navigable->set_needs_repaint();

    if (navigable->is_traversable()) {
        page().client().request_frame();
        return;
    }

    if (auto container = navigable->container()) {
        container->document().set_needs_repaint(InvalidateDisplayList::No);
    }
}

void Document::set_needs_accumulated_visual_contexts_update(bool value)
{
    m_needs_accumulated_visual_contexts_update = value;
    if (value)
        set_needs_repaint(InvalidateDisplayList::No);
}

void Document::schedule_full_accumulated_visual_context_rebuild(Layout::RustFFI::FfiVisualContextGlobalRebuildReason reason)
{
    if (m_layout_node_arena)
        Layout::RustFFI::layout_arena_visual_context_request_full_rebuild(m_layout_node_arena->handle(), reason);
    set_needs_accumulated_visual_contexts_update(true);
}

void Document::schedule_accumulated_visual_context_update(Layout::Node const& layout_node, AccumulatedVisualContextUpdateScope scope)
{
    if (!Painting::has_committed_box(layout_node))
        return;
    auto slot = Painting::committed_row_slot(layout_node);
    Layout::RustFFI::layout_arena_visual_context_note_box_dirty(
        layout_node_arena().handle(),
        slot,
        scope == AccumulatedVisualContextUpdateScope::Values
            ? Layout::RustFFI::FfiVisualContextBoxDirtyKind::StyleValueChange
            : Layout::RustFFI::FfiVisualContextBoxDirtyKind::StyleStructuralChange);

    set_needs_accumulated_visual_contexts_update(true);
}

void Document::schedule_accumulated_visual_context_update(Element& element, AccumulatedVisualContextUpdateScope scope)
{
    if (auto* layout_node = element.unsafe_layout_node())
        schedule_accumulated_visual_context_update(*layout_node, scope);
    element.for_each_synthetic_pseudo_element([&](CSS::PseudoElement, SyntheticPseudoElement const& pseudo_element) {
        if (auto* pseudo_element_layout_node = pseudo_element.unsafe_layout_node())
            schedule_accumulated_visual_context_update(*pseudo_element_layout_node, scope);
    });

    // https://drafts.csswg.org/css-backgrounds/#root-background
    // The root and body boxes paint each other's propagated background layers, so a structural
    // change on either has to reach both.
    if (scope != AccumulatedVisualContextUpdateScope::Structure)
        return;
    if (element.is_document_element()) {
        if (auto* body = this->body(); body && body->unsafe_layout_node())
            schedule_accumulated_visual_context_update(*body->unsafe_layout_node(), scope);
    } else if (&element == body()) {
        if (auto* document_element = this->document_element(); document_element && document_element->unsafe_layout_node())
            schedule_accumulated_visual_context_update(*document_element->unsafe_layout_node(), scope);
    }
}

void Document::schedule_scrollable_overflow_recalculation(Layout::Node const& layout_node)
{
    // SVG layout consumes transforms when computing geometry, so a transform change on SVG content
    // has to perform layout, matching the behavior of the transform presentation attribute.
    if (layout_node.is_svg_box()) {
        const_cast<Layout::Node&>(layout_node).set_needs_layout_update(DOM::SetNeedsLayoutReason::StyleChange);
        return;
    }

    Layout::RustFFI::layout_arena_schedule_scrollable_overflow_recalculation(layout_node.arena_handle(), Layout::Node::slot_id(&layout_node));
}

void Document::schedule_scrollable_overflow_recalculation(Element& element)
{
    if (auto* layout_node = element.unsafe_layout_node())
        schedule_scrollable_overflow_recalculation(*layout_node);
    element.for_each_synthetic_pseudo_element([&](CSS::PseudoElement, SyntheticPseudoElement const& pseudo_element) {
        if (auto* pseudo_element_layout_node = pseudo_element.unsafe_layout_node())
            schedule_scrollable_overflow_recalculation(*pseudo_element_layout_node);
    });
}

Painting::SnappedAreas const& Document::snapped_areas_of_scroll_container(Compositor::AsyncScrollNodeStableID const& stable_node_id) const
{
    static NeverDestroyed<Painting::SnappedAreas const> no_snapped_areas;
    auto snapped_areas = m_scroll_container_snapped_areas.find(stable_node_id);
    if (snapped_areas == m_scroll_container_snapped_areas.end())
        return *no_snapped_areas;
    return snapped_areas->value;
}

void Document::set_snapped_areas_of_scroll_container(Compositor::AsyncScrollNodeStableID const& stable_node_id, Painting::SnappedAreas snapped_areas)
{
    if (snapped_areas.is_empty()) {
        m_scroll_container_snapped_areas.remove(stable_node_id);
        return;
    }
    m_scroll_container_snapped_areas.set(stable_node_id, move(snapped_areas));
}

void Document::forget_snapped_areas_of_scroll_container(Layout::Node const& scroll_container)
{
    if (m_scroll_container_snapped_areas.is_empty())
        return;
    if (auto stable_node_id = Painting::async_scroll_node_stable_id(scroll_container); stable_node_id.has_value())
        m_scroll_container_snapped_areas.remove(*stable_node_id);
}

void Document::register_scroll_snap_container(Layout::Node const& snap_container)
{
    if (any_of(m_scroll_snap_containers, [&](auto const& registered) { return registered.ptr() == &snap_container; }))
        return;
    m_scroll_snap_containers.append(snap_container.make_weak_ptr());
}

Vector<NonnullRefPtr<Layout::Node const>> Document::collect_scroll_snap_containers()
{
    // A registered box whose layout node a style or layout update dropped is no longer a box of this document.
    m_scroll_snap_containers.remove_all_matching([](auto const& registered) {
        return !registered;
    });

    Vector<NonnullRefPtr<Layout::Node const>> snap_containers;
    snap_containers.ensure_capacity(m_scroll_snap_containers.size());
    for (auto const& registered : m_scroll_snap_containers) {
        // The scroll snap properties of a registered box can stop making it a snap container without its layout node
        // being rebuilt, and a registered box the latest commit left out has no committed box to snap with.
        if (Painting::has_committed_box(*registered) && Painting::is_scroll_snap_container(*registered))
            snap_containers.unchecked_append(*registered);
    }
    return snap_containers;
}

void Document::set_needs_to_record_display_list()
{
    m_hit_test_display_list = nullptr;
    if (auto navigable = this->navigable())
        navigable->set_needs_to_record_display_list();
}

RefPtr<Painting::DisplayList> Document::record_display_list(HTML::PaintConfig config, Painting::DisplayListResourceStorage& resource_storage, Painting::PaintCommandCacheMode cache_mode)
{
    update_paint_and_hit_testing_properties_if_needed();
    VERIFY(has_committed_viewport_box());

    bool const line_box_border_overlays_replace_cacheable_content = config.should_show_line_box_borders;
    if (line_box_border_overlays_replace_cacheable_content)
        cache_mode = Painting::PaintCommandCacheMode::ReadOnly;

    auto& document_paint_state = paint_state();
    auto visual_context_tree = document_paint_state.visual_context_tree(*this);

    auto placeholder_display_list = Painting::DisplayList::create(visual_context_tree);

    // https://drafts.csswg.org/css-color-adjust-1/#color-scheme-effect
    // On the root element, the used color scheme additionally must affect the surface color of the canvas, and the viewport’s scrollbars.
    if (navigable()->is_top_level_traversable()) {
        auto canvas_background_color = this->canvas_background_color();
        placeholder_display_list->set_surface_clear_color(canvas_background_color);
        page().client().page_did_change_background_color(canvas_background_color);
    }

    document_paint_state.refresh_scroll_state(*this);

    Painting::InspectorOverlayInputs overlay_inputs;
    if (auto const* layout_node = highlighted_layout_node(); layout_node && Painting::has_committed_box(*layout_node))
        overlay_inputs.highlighted_layout_node = layout_node;
    auto const& palette = page().palette();
    overlay_inputs.tooltip_color = palette.color(Gfx::ColorRole::Tooltip);
    overlay_inputs.tooltip_text_color = palette.color(Gfx::ColorRole::TooltipText);
    overlay_inputs.tooltip_border_color = palette.threed_shadow1();
    for (auto const& flexbox_highlight : m_flexbox_highlights) {
        if (auto const* layout_node = flexbox_highlight.node ? flexbox_highlight.node->unsafe_layout_node() : nullptr; layout_node && Painting::has_committed_box(*layout_node))
            overlay_inputs.flex_highlights.append({ layout_node, flexbox_highlight.options });
    }
    for (auto const& grid_highlight : m_grid_highlights) {
        if (auto const* layout_node = grid_highlight.node ? grid_highlight.node->unsafe_layout_node() : nullptr; layout_node && Painting::has_committed_box(*layout_node))
            overlay_inputs.grid_highlights.append({ layout_node, grid_highlight.options });
    }
    if (config.should_show_caret_hit_test_debug_overlay)
        overlay_inputs.caret_debug_rect = m_caret_hit_test_debug_rect;

    auto display_list = Painting::record_rust_display_list(*this, *placeholder_display_list, resource_storage, cache_mode, config, overlay_inputs);
    if (!display_list)
        return nullptr;

    bool const recording_returned_the_paint_command_cache_source = display_list == document_paint_state.display_list_used_as_paint_command_cache_source();
    if (!recording_returned_the_paint_command_cache_source || !m_hit_test_display_list || !m_hit_test_display_list->is_current())
        m_hit_test_display_list = Painting::HitTestDisplayList::create_from_rust_recording(visual_context_tree.structural_epoch(), layout_node_arena(), *m_chrome_widget_registry);

    if (cache_mode == Painting::PaintCommandCacheMode::ReadWrite && !recording_returned_the_paint_command_cache_source) {
        document_paint_state.set_display_list_used_as_paint_command_cache_source(display_list, resource_storage.collect_referenced_resources(*display_list));
    }

    return display_list;
}

void Document::set_caret_hit_test_debug_rect(Optional<CSSPixelRect> rect)
{
    if (m_caret_hit_test_debug_rect == rect)
        return;

    m_caret_hit_test_debug_rect = rect;
    set_needs_repaint(InvalidateDisplayList::Yes);
    page().client().request_frame();
}

Painting::HitTestDisplayList const* Document::ensure_hit_test_display_list()
{
    update_paint_and_hit_testing_properties_if_needed();

    if (!has_committed_viewport_box())
        return nullptr;

    auto rebuild_hit_test_display_list = [&] {
        set_needs_to_record_display_list();
        HTML::PaintConfig paint_config { .paint_overlay = true };
        if (auto navigable = this->navigable()) {
            if (navigable->record_display_list_and_scroll_state(paint_config))
                return;
            (void)record_display_list(paint_config, navigable->display_list_resource_storage(), Painting::PaintCommandCacheMode::ReadWrite);
            return;
        }
        Painting::DisplayListResourceStorage throwaway_resource_storage_for_hit_test_only_recording;
        (void)record_display_list(paint_config, throwaway_resource_storage_for_hit_test_only_recording, Painting::PaintCommandCacheMode::ReadOnly);
    };

    if (!m_hit_test_display_list || !m_hit_test_display_list->is_current() || m_hit_test_display_list->visual_context_tree_structural_epoch() != visual_context_tree_structural_epoch())
        rebuild_hit_test_display_list();

    return m_hit_test_display_list.ptr();
}

Optional<Painting::HitTestResult> Document::hit_test(CSSPixelPoint position)
{
    auto hit_test_display_list = ensure_hit_test_display_list();
    if (!hit_test_display_list)
        return {};
    paint_state().refresh_scroll_state(*this);
    // https://w3c.github.io/pointerevents/#hit-test
    // 1. Let pos be the x,y coordinates relative to the viewport
    // 2. Return [CSSOM-View]'s elementFromPoint() with pos (the frontmost DOM element at pos)

    // https://drafts.csswg.org/cssom-view/#dom-document-elementfrompoint
    // 1. If either argument is negative, x is greater than the viewport width excluding the size of a rendered scroll
    //    bar (if any), or y is greater than the viewport height excluding the size of a rendered scroll bar (if any),
    //    or there is no viewport associated with the document, return null and terminate these steps.
    // NB: The viewport is its own box in the display list, and its children are clipped to it.
    // 2. If there is a box in the viewport that would be a target for hit testing at coordinates x,y, when applying
    //    the transforms that apply to the descendants of the viewport, return the associated element and terminate
    //    these steps.
    auto result = hit_test_display_list->hit_test(position, *this, page().client().device_pixels_per_css_pixel(), page().chrome_metrics());
    if (result.has_value() && (result->chrome_widget || result->node))
        return result;

    // 3. If the document has a root element, return the root element and terminate these steps.
    // NB: Hit testing already redirects hits that fall through to the viewport onto the root element instead.

    // 4. Return null.
    return {};
}

Optional<Painting::CaretPosition> Document::caret_position_from_point(CSSPixelPoint position)
{
    auto hit_test_display_list = ensure_hit_test_display_list();
    if (!hit_test_display_list)
        return {};
    paint_state().refresh_scroll_state(*this);
    return hit_test_display_list->caret_position_from_point(position, *this, page().client().device_pixels_per_css_pixel(), page().chrome_metrics(), Painting::CaretPositionMode::Normal);
}

Optional<Painting::CaretPosition> Document::caret_position_from_point_for_selection_start(CSSPixelPoint position)
{
    auto hit_test_display_list = ensure_hit_test_display_list();
    if (!hit_test_display_list)
        return {};
    paint_state().refresh_scroll_state(*this);
    return hit_test_display_list->caret_position_from_point(position, *this, page().client().device_pixels_per_css_pixel(), page().chrome_metrics(), Painting::CaretPositionMode::SelectionStart);
}

Optional<Painting::CaretPosition> Document::caret_position_from_point_for_selection(CSSPixelPoint position, GC::Ptr<Node const> constraint_scope)
{
    auto hit_test_display_list = ensure_hit_test_display_list();
    if (!hit_test_display_list)
        return {};
    paint_state().refresh_scroll_state(*this);
    return hit_test_display_list->caret_position_from_point(position, *this, page().client().device_pixels_per_css_pixel(), page().chrome_metrics(), Painting::CaretPositionMode::Selection, constraint_scope);
}

Optional<Painting::CaretPosition> Document::caret_position_at_line_edge(Node const& node, size_t offset, TextAffinity affinity, Painting::CaretLineEdge edge)
{
    auto hit_test_display_list = ensure_hit_test_display_list();
    if (!hit_test_display_list)
        return {};
    paint_state().refresh_scroll_state(*this);
    return hit_test_display_list->caret_position_at_line_edge(node, offset, affinity, edge);
}

Optional<Painting::CaretPosition> Document::caret_position_on_adjacent_line(Node const& node, size_t offset, TextAffinity affinity, Painting::CaretLineDirection direction, CSSPixels inline_coordinate, Node const& scope)
{
    auto hit_test_display_list = ensure_hit_test_display_list();
    if (!hit_test_display_list)
        return {};
    paint_state().refresh_scroll_state(*this);
    return hit_test_display_list->caret_position_on_adjacent_line(node, offset, affinity, direction, inline_coordinate, scope);
}

Optional<CSSPixels> Document::caret_line_block_coordinate(Node const& node, size_t offset, TextAffinity affinity)
{
    auto hit_test_display_list = ensure_hit_test_display_list();
    if (!hit_test_display_list)
        return {};
    paint_state().refresh_scroll_state(*this);
    return hit_test_display_list->caret_line_block_coordinate(node, offset, affinity);
}

TraversalDecision Document::hit_test_all(CSSPixelPoint position, Function<TraversalDecision(Painting::HitTestResult)> const& callback)
{
    auto hit_test_display_list = ensure_hit_test_display_list();
    if (!hit_test_display_list)
        return TraversalDecision::Continue;
    paint_state().refresh_scroll_state(*this);
    return hit_test_display_list->hit_test_all(position, *this, page().client().device_pixels_per_css_pixel(), page().chrome_metrics(), callback);
}

Unicode::Segmenter& Document::grapheme_segmenter() const
{
    if (!m_grapheme_segmenter)
        m_grapheme_segmenter = Unicode::Segmenter::create(Unicode::SegmenterGranularity::Grapheme);
    return *m_grapheme_segmenter;
}

Unicode::Segmenter& Document::line_segmenter() const
{
    if (!m_line_segmenter)
        m_line_segmenter = Unicode::Segmenter::create(Unicode::SegmenterGranularity::Line);
    return *m_line_segmenter;
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
    auto& window = HTML::relevant_window(*this);
    auto beforeunload_event = HTML::BeforeUnloadEvent::create(HTML::EventNames::beforeunload, {}, HighResolutionTime::current_high_resolution_time(relevant_global_object(*this)));
    beforeunload_event->set_cancelable(true);
    auto event_firing_result = window.dispatch_event(*beforeunload_event);

    // 5. Decrease document's relevant agent's event loop's termination nesting level by 1.
    event_loop.decrement_termination_nesting_level();

    // 6. If all of the following are true:
    if (
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
        // 1. Set unloadPromptShown to true.
        unload_prompt_shown = true;

        // FIXME: 2. Invoke WebDriver BiDi user prompt opened with document's relevant global object, "beforeunload", and "".
        // FIXME: 3. Ask the user to confirm that they wish to unload the document, and pause while waiting for the user's response.

        auto user_prompt_handler = WebDriver::get_the_prompt_handler(WebDriver::PromptType::BeforeUnload);

        // 4. If the user did not confirm the page navigation, set unloadPromptCanceled to true.
        if (user_prompt_handler.handler == WebDriver::PromptHandler::Dismiss)
            unload_prompt_canceled = true;

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

// https://dom.spec.whatwg.org/#flatten-element-creation-options
WebIDL::ExceptionOr<Document::RegistryAndIs> Document::flatten_element_creation_options(ElementCreationOptions const& options) const
{
    // 1. Let registry be the result of looking up a custom element registry given document.
    GC::Ptr<HTML::CustomElementRegistry> registry = custom_element_registry();

    // 2. Let is be null.
    Optional<Utf16FlyString> is;

    // 3. If options["is"] exists, then set is to it.
    if (options.is.has_value())
        is = options.is;

    // 4. If options["customElementRegistry"] exists:
    if (options.custom_element_registry.has_value()) {
        // 1. If is is non-null, then throw a "NotSupportedError" DOMException.
        if (is.has_value())
            return WebIDL::NotSupportedError::create("Cannot specify both 'is' and 'customElementRegistry' in ElementCreationOptions."_utf16);

        // 2. Set registry to options["customElementRegistry"].
        registry = options.custom_element_registry.value();
    }

    // 5. If registry is non-null, registry’s is scoped is false, and registry is not document’s custom element
    //    registry, then throw a "NotSupportedError" DOMException.
    if (registry && !registry->is_scoped() && registry != custom_element_registry())
        return WebIDL::NotSupportedError::create("'customElementRegistry' in ElementCreationOptions must either be scoped or the document's custom element registry."_utf16);

    // 6. Return registry and is.
    return RegistryAndIs { registry, move(is) };
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

WebIDL::CallbackType* Document::onfullscreenchange()
{
    return event_handler_attribute(HTML::EventNames::fullscreenchange);
}

void Document::set_onfullscreenchange(WebIDL::CallbackType* value)
{
    set_event_handler_attribute(HTML::EventNames::fullscreenchange, value);
}

WebIDL::CallbackType* Document::onfullscreenerror()
{
    return event_handler_attribute(HTML::EventNames::fullscreenerror);
}

void Document::set_onfullscreenerror(WebIDL::CallbackType* value)
{
    set_event_handler_attribute(HTML::EventNames::fullscreenerror, value);
}

WebIDL::CallbackType* Document::onwebkitfullscreenchange()
{
    return event_handler_attribute(HTML::EventNames::webkitfullscreenchange);
}

void Document::set_onwebkitfullscreenchange(WebIDL::CallbackType* value)
{
    set_event_handler_attribute(HTML::EventNames::webkitfullscreenchange, value);
}

WebIDL::CallbackType* Document::onwebkitfullscreenerror()
{
    return event_handler_attribute(HTML::EventNames::webkitfullscreenerror);
}

void Document::set_onwebkitfullscreenerror(WebIDL::CallbackType* value)
{
    set_event_handler_attribute(HTML::EventNames::webkitfullscreenerror, value);
}

// https://drafts.csswg.org/css-view-transitions-1/#dom-document-startviewtransition
GC::Ptr<ViewTransition::ViewTransition> Document::start_view_transition(GC::Ptr<WebIDL::CallbackType> update_callback, GC::Ref<WebIDL::Promise> ready_promise, GC::Ref<WebIDL::Promise> update_callback_done_promise, GC::Ref<WebIDL::Promise> finished_promise)
{
    // The method steps for startViewTransition(updateCallback) are as follows:

    // 1. Let transition be a new ViewTransition object in this’s relevant Realm.
    auto transition = ViewTransition::ViewTransition::create(GC::Ref { *this }, ready_promise, update_callback_done_promise, finished_promise);

    // 2. If updateCallback is provided, set transition’s update callback to updateCallback.
    if (update_callback != nullptr)
        transition->set_update_callback(update_callback);

    // 3. Let document be this’s relevant global object’s associated document.
    auto& document = HTML::relevant_window(*this).associated_document();

    // 4. If document’s visibility state is "hidden", then skip transition with an "InvalidStateError" DOMException,
    //    and return transition.
    if (m_visibility_state == HTML::VisibilityState::Hidden) {
        transition->skip_the_view_transition(WebIDL::InvalidStateError::create("The document's visibility state is \"hidden\""_utf16));
        return transition;
    }

    // 5. If document’s active view transition is not null, then skip that view transition with an "AbortError"
    //    DOMException in this’s relevant Realm.
    if (document.m_active_view_transition)
        document.m_active_view_transition->skip_the_view_transition(WebIDL::AbortError::create("Document.startViewTransition() was called"_utf16));

    // 6. Set document’s active view transition to transition.
    m_active_view_transition = transition;

    // 7. Return transition.
    return transition;
}

// https://drafts.csswg.org/css-view-transitions-1/#perform-pending-transition-operations
void Document::perform_pending_transition_operations()
{
    // To perform pending transition operations given a Document document, perform the following steps:

    // 1. If document’s active view transition is not null, then:
    if (m_active_view_transition) {
        // 1. If document’s active view transition’s phase is "pending-capture", then setup view transition for
        //    document’s active view transition.
        if (m_active_view_transition->phase() == ViewTransition::ViewTransition::Phase::PendingCapture)
            m_active_view_transition->setup_view_transition();
        // 2. Otherwise, if document’s active view transition’s phase is "animating", then handle transition frame for
        //    document’s active view transition.
        else if (m_active_view_transition->phase() == ViewTransition::ViewTransition::Phase::Animating)
            m_active_view_transition->handle_transition_frame();
    }
}

// https://drafts.csswg.org/css-view-transitions-1/#flush-the-update-callback-queue
void Document::flush_the_update_callback_queue()
{
    // To flush the update callback queue given a Document document:

    // 1. For each transition in document’s update callback queue, call the update callback given transition.
    for (auto& transition : m_update_callback_queue) {
        transition->call_the_update_callback();
    }

    // 2. Set document’s update callback queue to an empty list.
    m_update_callback_queue.clear();
}

void Document::set_rendering_suppression_for_view_transitions(bool value)
{
    if (m_rendering_suppression_for_view_transitions == value)
        return;

    m_rendering_suppression_for_view_transitions = value;

    if (!value)
        page().client().request_frame();
}

// https://drafts.csswg.org/css-view-transitions-1/#view-transition-page-visibility-change-steps
void Document::view_transition_page_visibility_change_steps()
{
    // The view transition page-visibility change steps given Document document are:

    // 1. Queue a global task on the DOM manipulation task source, given document’s relevant global object, to
    //    perform the following steps:
    HTML::queue_global_task(HTML::Task::Source::DOMManipulation, HTML::relevant_global_object(*this), GC::create_function(GC::Heap::the(), [&] {
        HTML::TemporaryExecutionContext context(relevant_settings_object());
        // 1. If document’s visibility state is "hidden", then:
        if (m_visibility_state == HTML::VisibilityState::Hidden) {
            // 1. If document’s active view transition is not null, then skip document’s active view transition with an
            //    "InvalidStateError" DOMException.
            if (m_active_view_transition) {
                m_active_view_transition->skip_the_view_transition(WebIDL::InvalidStateError::create("The document's visibility state is \"hidden\"."_utf16));
            }
        }
        // 2. Otherwise, assert: active view transition is null.
        else {
            VERIFY(!m_active_view_transition);
        }
    }));
}

// https://dom.spec.whatwg.org/#dom-documentorshadowroot-customelementregistry
GC::Ptr<HTML::CustomElementRegistry> Document::custom_element_registry() const
{
    // 1. If this is a document, then return this’s custom element registry.
    // NB: Always true.
    return m_custom_element_registry;

    // 2. Assert: this is a ShadowRoot node.
    // 3. Return this’s custom element registry.
}

// https://dom.spec.whatwg.org/#effective-global-custom-element-registry
GC::Ptr<HTML::CustomElementRegistry> Document::effective_global_custom_element_registry() const
{
    // A document document’s effective global custom element registry is:

    // 1. If document’s custom element registry is a global custom element registry, then return document’s custom
    //    element registry.
    if (HTML::is_a_global_custom_element_registry(custom_element_registry()))
        return custom_element_registry();

    // 2. Return null.
    return nullptr;
}

// https://html.spec.whatwg.org/multipage/custom-elements.html#upgrade-particular-elements-within-a-document
void Document::upgrade_particular_elements(GC::Ref<HTML::CustomElementRegistry> registry, GC::Ref<HTML::CustomElementDefinition> definition, Utf16FlyString local_name, Optional<Utf16FlyString> maybe_name)
{
    // To upgrade particular elements within a document given a CustomElementRegistry object registry, a Document
    // object document, a custom element definition definition, a string localName, and optionally a string name
    // (default localName):
    auto name = maybe_name.value_or(local_name);
    auto only_include_elements_with_matching_is_value = name != local_name;

    // 1. Let upgradeCandidates be all elements that are shadow-including descendants of document, whose custom element
    //    registry is registry, whose namespace is the HTML namespace, and whose local name is localName, in
    //    shadow-including tree order.
    //    Additionally, if name is not localName, only include elements whose is value is equal to name.
    for_each_shadow_including_descendant([&](Node& inclusive_descendant) {
        auto* element = as_if<Element>(inclusive_descendant);
        if (!element
            || element->custom_element_registry() != registry
            || element->namespace_uri() != Namespace::HTML
            || element->local_name() != local_name) {

            return TraversalDecision::Continue;
        }

        if (only_include_elements_with_matching_is_value && (!element->is_value().has_value() || element->is_value().value() != name))
            return TraversalDecision::Continue;

        // 2. For each element element of upgradeCandidates:
        //    enqueue a custom element upgrade reaction given element and definition.
        element->enqueue_a_custom_element_upgrade_reaction(definition);
        return TraversalDecision::Continue;
    });
}

ElementByIdMap& Document::element_by_id() const
{
    if (!m_element_by_id)
        m_element_by_id = make<ElementByIdMap>();
    return *m_element_by_id;
}

Utf16String Document::dump_display_list()
{
    update_layout(UpdateLayoutReason::DumpDisplayList);

    if (!has_committed_viewport_box())
        return "No paintable"_utf16;

    if (paint_state().has_visual_context_tree())
        schedule_full_accumulated_visual_context_rebuild(Layout::RustFFI::FfiVisualContextGlobalRebuildReason::CanonicalDumpRequested);

    auto& resource_storage = navigable()->display_list_resource_storage();
    auto display_list = record_display_list(HTML::PaintConfig {}, resource_storage, Painting::PaintCommandCacheMode::ReadOnly);
    if (!display_list)
        return "No display list"_utf16;

    auto visual_context_tree = paint_state().visual_context_tree(*this);
    return Painting::serialize_painting_dump(*this, visual_context_tree, *display_list, resource_storage);
}

Utf16String Document::dump_stacking_context_tree()
{
    update_layout(UpdateLayoutReason::DumpDisplayList);

    if (!has_committed_viewport_box())
        return "No paintable"_utf16;

    update_paint_and_hit_testing_properties_if_needed();

    StringBuilder builder;
    Painting::dump_stacking_context_tree(builder, *this);
    if (builder.is_empty())
        return "No stacking context"_utf16;
    return Utf16String::from_utf8_without_validation(builder.string_view());
}

HashMap<Utf16FlyString, CSS::CustomPropertyRegistration>& Document::registered_property_set()
{
    return m_registered_property_set;
}

WebIDL::ExceptionOr<GC::Ref<XPath::XPathExpression>> Document::create_expression(Utf16String const& expression, GC::Ptr<XPath::XPathNSResolver> resolver)
{
    return XPath::create_expression(expression, resolver);
}

WebIDL::ExceptionOr<GC::Ref<XPath::XPathResult>> Document::evaluate(Utf16String const& expression, DOM::Node const& context_node, GC::Ptr<XPath::XPathNSResolver> resolver, WebIDL::UnsignedShort type, GC::Ptr<XPath::XPathResult> result)
{
    return XPath::throw_evaluation_error_if_needed(XPath::evaluate(expression, context_node, resolver, type, result));
}

GC::Ref<DOM::Node> Document::create_ns_resolver(GC::Ref<DOM::Node> node_resolver)
{
    return node_resolver;
}

// https://drafts.css-houdini.org/css-properties-values-api/#determining-registration
Optional<CSS::CustomPropertyRegistration const&> Document::get_registered_custom_property(Utf16FlyString const& name) const
{
    // If the Document’s [[registeredPropertySet]] slot contains a record with the custom property’s name, the
    // registration is that record.
    if (auto registered_property = m_registered_property_set.get(name); registered_property.has_value())
        return registered_property;

    // Otherwise, if the Document’s active stylesheets contain at least one valid @property rule representing a
    // registration with the custom property’s name, the last such one in document order is the registration.
    if (auto registered_property = m_cached_registered_properties_from_css_property_rules.get(name); registered_property.has_value())
        return registered_property;

    // Otherwise there is no registration, and the custom property is not a registered custom property.
    return {};
}

void Document::did_change_custom_property_registrations()
{
    ++m_custom_property_registration_generation;
    sync_custom_property_registrations_to_rust();

    // Custom property registration changes can alter inheritance and initial values even when no selector matching
    // changes. Registrations only move when a stylesheet containing an @property rule is added/removed or when
    // CSS.registerProperty() is called, so a full document restyle is cheap enough in practice.
    record_style_environment_change();
}

void Document::sync_custom_property_registrations_to_rust()
{
    HashMap<Utf16FlyString, CSS::CustomPropertyRegistration const*> effective_registrations;
    effective_registrations.ensure_capacity(m_registered_property_set.size() + m_cached_registered_properties_from_css_property_rules.size());
    for (auto const& [name, registration] : m_cached_registered_properties_from_css_property_rules)
        effective_registrations.set(name, &registration);
    for (auto const& [name, registration] : m_registered_property_set)
        effective_registrations.set(name, &registration);

    Vector<Utf16String> names;
    Vector<Optional<Utf16String>> initial_values;
    Vector<CSS::ComputedValuesFFI::FfiCustomPropertyRegistration> registrations;
    names.ensure_capacity(effective_registrations.size());
    initial_values.ensure_capacity(effective_registrations.size());
    registrations.ensure_capacity(effective_registrations.size());
    for (auto const& [name, registration] : effective_registrations) {
        names.unchecked_append(name.to_utf16_string());
        initial_values.unchecked_append(registration->initial_value
                ? Optional<Utf16String> { registration->initial_value->to_utf16_string(CSS::SerializationMode::ResolvedValueForReparse) }
                : Optional<Utf16String> {});
        auto const& name_string = names.last();
        auto const& initial_value = initial_values.last();
        registrations.unchecked_append({
            .name = ffi_utf16_view(name_string),
            .syntax = registration->syntax.data(),
            .inherits = registration->inherit,
            .has_initial_value = initial_value.has_value(),
            .initial_value = initial_value.has_value() ? ffi_utf16_view(*initial_value) : CSS::ComputedValuesFFI::FfiUtf16View {},
        });
    }
    auto document_url = url().serialize();
    auto document_base_url = base_url().serialize();
    CSS::ComputedValuesFFI::FfiCustomPropertyRegistryContext context {
        .document_url = document_url.bytes().data(),
        .document_url_length = document_url.bytes().size(),
        .document_base_url = document_base_url.bytes().data(),
        .document_base_url_length = document_base_url.bytes().size(),
        .intern_utf16_fly_string = retain_registered_property_utf16_fly_string,
    };
    CSS::ComputedValuesFFI::rust_custom_property_registry_update(
        m_rust_custom_property_registry, &context, registrations.data(), registrations.size());
}

void Document::build_registered_properties_cache()
{
    // The set of effective @property rules can only change when the active stylesheet rule set changes, which also
    // invalidates the document's style cache. Skip the rebuild until that happens.
    if (!m_needs_registered_properties_cache_update)
        return;
    m_needs_registered_properties_cache_update = false;

    ++m_style_invalidation_counters.registered_properties_cache_rebuilds;

    HashMap<Utf16FlyString, CSS::CustomPropertyRegistration> cached_registered_properties_from_css_property_rules;
    for_each_active_css_style_sheet([&](CSS::CSSStyleSheet const& style_sheet) {
        style_sheet.for_each_effective_rule(TraversalOrder::Preorder, [&](CSS::CSSRule const& rule) {
            if (auto* property_rule = as_if<CSS::CSSPropertyRule>(rule))
                cached_registered_properties_from_css_property_rules.set(property_rule->name(), property_rule->to_registration());
        });
    });

    auto registrations_changed = cached_registered_properties_from_css_property_rules != m_cached_registered_properties_from_css_property_rules;
    m_cached_registered_properties_from_css_property_rules = move(cached_registered_properties_from_css_property_rules);
    if (registrations_changed)
        did_change_custom_property_registrations();
}

void Document::ensure_cookie_version_index(URL::URL const& new_url, URL::URL const& old_url)
{
    auto new_domain = HTTP::Cookie::canonicalize_domain(new_url);
    if (!new_domain.has_value()) {
        m_cookie_version_index = {};
        return;
    }

    if (m_cookie_version_index.has_value() && *new_domain == HTTP::Cookie::canonicalize_domain(old_url))
        return;

    page().client().page_did_request_document_cookie_version_index(unique_id(), *new_domain);
    m_cookie_version_index = {};
}

// https://html.spec.whatwg.org/multipage/dom.html#internal-ancestor-origin-objects-list-creation-steps
Vector<URL::Origin> Document::internal_ancestor_origin_objects_list_creation_steps(ReferrerPolicy::ReferrerPolicy referrer_policy) const
{
    // 1. Let output be « ».
    Vector<URL::Origin> output;

    // 2. Let parentDoc be document's container document.
    auto parent_doc = container_document();

    // 3. If parentDoc is null, then return output.
    if (!parent_doc)
        return output;

    // 4. Assert: parentDoc is fully active.
    VERIFY(parent_doc->is_fully_active());

    // 5. Let ancestorOrigins be parentDoc's internal ancestor origin objects list.
    auto ancestor_origins = parent_doc->internal_ancestor_origin_objects_list();

    // 6. Let container be document's node navigable's container.
    // AD-HOC: This isn't used, see https://github.com/whatwg/html/issues/12566
    // auto container = navigable()->container();

    // 7. Let masked be false.
    auto masked = false;

    // 8. If referrerPolicy is "no-referrer", then set masked to true.
    if (referrer_policy == ReferrerPolicy::ReferrerPolicy::NoReferrer)
        masked = true;

    // 9. Otherwise, if referrerPolicy is "same-origin" and parentDoc's origin is not same origin with document's origin, then set masked to true.
    else if (referrer_policy == ReferrerPolicy::ReferrerPolicy::SameOrigin && parent_doc->origin().is_same_origin(origin()))
        masked = true;

    // 10. If masked is true, then append a new opaque origin to output.
    if (masked)
        output.append(URL::Origin::create_opaque());

    // 11. Otherwise, append parentDoc's origin to output.
    else
        output.append(parent_doc->origin());

    // 12. For each ancestorOrigin of ancestorOrigins:
    for (auto const& ancestor_origin : ancestor_origins.value()) {
        // 1. If masked is true and ancestorOrigin is same origin with parentDoc's origin, then append a new opaque origin to output and continue.
        if (masked && ancestor_origin.is_same_origin(parent_doc->origin())) {
            output.append(URL::Origin::create_opaque());
            continue;
        }

        // 2. Append ancestorOrigin to output and set masked to false.
        output.append(ancestor_origin);
        masked = false;
    }

    // 13. Return output.
    return output;
}

// https://html.spec.whatwg.org/multipage/dom.html#ancestor-origins-list-creation-steps
GC::Ref<HTML::DOMStringList> Document::ancestor_origins_list_creation_steps() const
{
    // 1. Let ancestorOrigins be document's internal ancestor origin objects list.
    auto& ancestor_origins = m_internal_ancestor_origin_objects_list;

    // 2. Assert: ancestorOrigins is not null.
    VERIFY(ancestor_origins.has_value());

    // 3. Let output be « ».
    Vector<String> output;

    // 4. For each origin of ancestorOrigins:
    for (auto const& origin : ancestor_origins.value()) {
        // 1. Append the serialization of origin to output.
        output.append(origin.serialize());
    }

    // 5. Return a new DOMStringList object whose associated list is output.
    return HTML::DOMStringList::create(move(output));
}

Utf16View to_string(SetNeedsLayoutReason reason)
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

Utf16View to_string(SetNeedsLayoutTreeUpdateReason reason)
{
    switch (reason) {
#define ENUMERATE_SET_NEEDS_LAYOUT_TREE_UPDATE_REASON(e) \
    case SetNeedsLayoutTreeUpdateReason::e:              \
        return #e##sv;
        ENUMERATE_SET_NEEDS_LAYOUT_TREE_UPDATE_REASONS(ENUMERATE_SET_NEEDS_LAYOUT_TREE_UPDATE_REASON)
#undef ENUMERATE_SET_NEEDS_LAYOUT_TREE_UPDATE_REASON
    }
    VERIFY_NOT_REACHED();
}

Utf16View to_string(InvalidateLayoutTreeReason reason)
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

Utf16View to_string(UpdateLayoutReason reason)
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

Utf16View to_string(PartialRelayoutEscapeReason reason)
{
    switch (reason) {
#define ENUMERATE_PARTIAL_RELAYOUT_ESCAPE_REASON(e) \
    case PartialRelayoutEscapeReason::e:            \
        return #e##sv;
        ENUMERATE_PARTIAL_RELAYOUT_ESCAPE_REASONS(ENUMERATE_PARTIAL_RELAYOUT_ESCAPE_REASON)
#undef ENUMERATE_PARTIAL_RELAYOUT_ESCAPE_REASON
    }
    VERIFY_NOT_REACHED();
}

Utf16View to_string(PartialRelayoutEscapeClearReason reason)
{
    switch (reason) {
#define ENUMERATE_PARTIAL_RELAYOUT_ESCAPE_CLEAR_REASON(e) \
    case PartialRelayoutEscapeClearReason::e:             \
        return #e##sv;
        ENUMERATE_PARTIAL_RELAYOUT_ESCAPE_CLEAR_REASONS(ENUMERATE_PARTIAL_RELAYOUT_ESCAPE_CLEAR_REASON)
#undef ENUMERATE_PARTIAL_RELAYOUT_ESCAPE_CLEAR_REASON
    }
    VERIFY_NOT_REACHED();
}

void Document::add_pending_css_import_rule(Badge<CSS::CSSImportRule>, GC::Ref<CSS::CSSImportRule> rule)
{
    m_pending_css_import_rules.set(rule);
}

void Document::remove_pending_css_import_rule(Badge<CSS::CSSImportRule>, GC::Ref<CSS::CSSImportRule> rule)
{
    m_pending_css_import_rules.remove(rule);
    if (m_pending_css_import_rules.is_empty())
        page().client().request_frame();
}

void Document::exit_pointer_lock()
{
    dbgln("FIXME: exit_pointer_lock()");
}

static bool contains_named_namespace(CSS::SelectorList const& selectors)
{
    return selectors.contains([](auto const& selector) { return selector->contains_named_namespace(); });
}

RefPtr<SelectorQuery const> Document::selector_query_for(Utf16View selector_text) const
{
    static constexpr size_t MAX_SELECTOR_QUERY_CACHE_SIZE = 512;

    if (m_last_selector_query_text.has_value() && selector_text == *m_last_selector_query_text)
        return m_last_selector_query;

    if (auto it = m_selector_query_cache.find(selector_text); it != m_selector_query_cache.end()) {
        m_last_selector_query_text = it->key;
        m_last_selector_query = it->value;
        return it->value;
    }

    auto maybe_selectors = parse_selector(CSS::Parser::ParsingParams { *this }, selector_text);

    // "Note: Support for namespaces within selectors is not planned and will not be added."
    if (maybe_selectors.has_value() && contains_named_namespace(maybe_selectors.value()))
        maybe_selectors.clear();

    RefPtr<SelectorQuery const> query;
    if (maybe_selectors.has_value())
        query = SelectorQuery::create(const_cast<Document&>(*this), maybe_selectors.release_value());

    if (m_selector_query_cache.size() >= MAX_SELECTOR_QUERY_CACHE_SIZE)
        m_selector_query_cache.remove(m_selector_query_cache.begin());

    auto selector_text_copy = Utf16String::from_utf16(selector_text);
    m_last_selector_query_text = selector_text_copy;
    m_last_selector_query = query;
    m_selector_query_cache.set(move(selector_text_copy), query);
    return query;
}

QuerySelectorResultCache& Document::query_selector_result_cache()
{
    if (!m_query_selector_result_cache)
        m_query_selector_result_cache = make<QuerySelectorResultCache>();
    return *m_query_selector_result_cache;
}

}

namespace Web::Bindings {

JS::Value document(JS::Realm& realm, GC::Ref<DOM::Document> document)
{
    return wrap(host_defined_wrapper_world(realm), realm, document);
}

GC::Ref<CSS::StyleSheetList> style_sheets(DOM::Document& document)
{
    return document.style_sheets();
}

JS::Value adopted_style_sheets(DOM::Document& document)
{
    return DOM::AdoptedStyleSheetsAccess::adopted_style_sheets(document).ptr();
}

WebIDL::ExceptionOr<void> set_adopted_style_sheets(DOM::Document& document, JS::Value new_value)
{
    auto adopted_style_sheets = DOM::AdoptedStyleSheetsAccess::adopted_style_sheets(document);
    adopted_style_sheets->clear();

    auto iterator_record = TRY(JS::get_iterator(document.vm(), new_value, JS::IteratorHint::Sync));
    while (true) {
        auto next = TRY(JS::iterator_step_value(document.vm(), iterator_record));
        if (!next.has_value())
            break;
        TRY(adopted_style_sheets->append(*next));
    }

    return {};
}

WebIDL::ExceptionOr<GC::Root<DOM::Document>> parse_html_unsafe(JS::Realm& realm, TrustedTypes::TrustedHTMLOrString const& html)
{
    // FIXME: update description once https://github.com/whatwg/html/issues/11778 gets solved
    // 1. Let compliantHTML to the result of invoking the Get Trusted Type compliant string algorithm with
    //    TrustedHTML, this's relevant global object, html, "Document parseHTMLUnsafe", and "script".
    auto const compliant_html = TRY(TrustedTypes::get_trusted_type_compliant_string(
        TrustedTypes::TrustedTypeName::TrustedHTML,
        HTML::current_global_object(),
        html,
        TrustedTypes::InjectionSink::Document_parseHTMLUnsafe,
        "script"_utf16));

    // AD-HOC: Setting the origin to match that of the associated document matches the behavior of existing browsers.
    auto* window = HTML::window_from_global_object(realm.global_object());
    VERIFY(window);

    // 2. Let document be a new Document, whose content type is "text/html".
    auto document = DOM::Document::create_for_fragment_parsing(window->page(), *window);
    document->set_content_type("text/html"_utf16_fly_string);

    // 3. Set document's allow declarative shadow roots to true.
    document->set_allow_declarative_shadow_roots(HTML::HTMLParser::AllowDeclarativeShadowRoots::Yes);

    // 4. Parse HTML from a string given document and compliantHTML.
    [[maybe_unused]] auto wrapper = wrap(host_defined_wrapper_world(realm), realm, GC::Ref { *document });
    document->parse_html_from_a_string(compliant_html.utf16_view());

    auto& associated_document = window->associated_document();
    document->set_origin(associated_document.origin());

    // 5. Return document.
    return document;
}

static WebIDL::ExceptionOr<Utf16String> document_write_compliant_string(DOM::Document& document, Vector<TrustedTypes::TrustedHTMLOrString> const& text, TrustedTypes::InjectionSink sink)
{
    // 1. Let string be the empty string.
    Utf16StringBuilder string;

    // 2. Let isTrusted be false if text contains a string; otherwise true.
    auto is_trusted = true;
    for (auto const& value : text) {
        if (value.has<Utf16String>()) {
            is_trusted = false;
            break;
        }
    }

    // 3. For each value of text:
    for (auto const& value : text) {
        string.append(value.visit(
            // 1. If value is a TrustedHTML object, then append value's associated data to string.
            [](GC::Root<TrustedTypes::TrustedHTML> const& value) -> Utf16String const& { return value->to_string(); },
            // 2. Otherwise, append value to string.
            [](Utf16String const& value) -> Utf16String const& { return value; }));
    }

    // 4. If isTrusted is false, set string to the result of invoking the Get Trusted Type compliant string algorithm
    //    with TrustedHTML, this's relevant global object, string, sink, and "script".
    if (!is_trusted) {
        auto const new_string = TRY(TrustedTypes::get_trusted_type_compliant_string(
            TrustedTypes::TrustedTypeName::TrustedHTML,
            HTML::relevant_global_object(document),
            string.to_string(),
            sink,
            "script"_utf16));
        string.clear();
        string.append(new_string);
    }

    return string.to_string();
}

WebIDL::ExceptionOr<void> write(DOM::Document& document, Vector<TrustedTypes::TrustedHTMLOrString> const& text)
{
    return document.write(TRY(document_write_compliant_string(document, text, TrustedTypes::InjectionSink::Document_write)));
}

WebIDL::ExceptionOr<void> writeln(DOM::Document& document, Vector<TrustedTypes::TrustedHTMLOrString> const& text)
{
    return document.writeln(TRY(document_write_compliant_string(document, text, TrustedTypes::InjectionSink::Document_writeln)));
}

GC::Ptr<ViewTransition::ViewTransition> start_view_transition(DOM::Document& document, GC::Ptr<WebIDL::CallbackType> update_callback)
{
    return document.start_view_transition(
        update_callback,
        WebIDL::create_promise_for(document),
        WebIDL::create_promise_for(document),
        WebIDL::create_promise_for(document));
}

// https://html.spec.whatwg.org/multipage/dom.html#dom-document-nameditem
JS::Value document_named_item_value(WrapperWorld& wrapper_world, JS::Realm& realm, DOM::Document const& document, Utf16FlyString const& name)
{
    // 1. Let elements be the list of named elements with the name name that are in a document tree with the Document as their root.
    // NOTE: There will be at least one such element, since the algorithm would otherwise not have been invoked by Web IDL.
    auto elements = document.named_elements_with_name(name);

    // 2. If elements has only one element, and that element is an iframe element, and that iframe element's content navigable is not null,
    //    then return the active WindowProxy of the element's content navigable.
    if (elements.size() == 1 && is<HTML::HTMLIFrameElement>(*elements.first())) {
        auto& iframe_element = static_cast<HTML::HTMLIFrameElement&>(*elements.first());
        if (iframe_element.content_navigable() != nullptr)
            return iframe_element.content_navigable()->active_window_proxy();
    }

    // 3. Otherwise, if elements has only one element, return that element.
    if (elements.size() == 1)
        return wrap(wrapper_world, realm, elements.first()).ptr();

    // 4. Otherwise return an HTMLCollection rooted at the Document node, whose filter matches only named elements with the name name.
    auto collection = DOM::HTMLCollection::create(*const_cast<DOM::Document*>(&document), DOM::HTMLCollection::Scope::Descendants, [name](auto& element) { return DOM::Document::is_named_element_with_name(element, name); }, DOM::HTMLCollection::AttributeInvalidationType::IdOrName);
    return wrap(wrapper_world, realm, collection);
}

}
