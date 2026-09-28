/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/CustomElements/CustomElementRegistry.h>
#include <LibWeb/HTML/HTMLDocument.h>
#include <LibWeb/HTML/HTMLIFrameElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/LocalTraversableNavigable.h>
#include <LibWeb/HTML/RemoteNavigable.h>
#include <LibWeb/HTML/Scripting/WindowEnvironmentSettingsObject.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/Infra/SerializedURL.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Page/Page.h>
#include <LibWebCommon/HTML/SandboxingFlagSet.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(BrowsingContext);

// https://html.spec.whatwg.org/multipage/document-sequences.html#creating-a-new-auxiliary-browsing-context
BrowsingContext::BrowsingContextAndDocument BrowsingContext::create_a_new_auxiliary_browsing_context_and_document(GC::Ref<Page> page, GC::Ref<HTML::BrowsingContext> opener)
{
    // 1. Let openerTopLevelBrowsingContext be opener's top-level traversable's active browsing context.
    // 2. Let group be openerTopLevelBrowsingContext's group.
    // 3. Assert: group is non-null, as navigating invokes this directly.
    // NB: The UI process holds the group.

    // 4. Set browsingContext and document be the result of creating a new browsing context and document with opener's active document, null, and group.
    auto [browsing_context, document] = create_a_new_browsing_context_and_document(page, opener->active_document(), nullptr);

    // 5. Set browsingContext's is auxiliary to true.
    browsing_context->m_is_auxiliary = true;

    // 6. Append browsingContext to group.
    // NB: The UI process appends its browsing context to the group.

    // 7. Set browsingContext's opener browsing context to opener.
    browsing_context->set_opener_browsing_context(opener);

    // 8. Set browsingContext's virtual browsing context group ID to openerTopLevelBrowsingContext's virtual browsing context group ID.
    // 9. Set browsingContext's opener origin at creation to opener's active document's origin.
    // NB: The UI process holds these on the canonical browsing context.

    // 10. Return browsingContext and document.
    return BrowsingContext::BrowsingContextAndDocument { browsing_context, document };
}

static void populate_with_html_head_body(GC::Ref<DOM::Document> document)
{
    auto html_node = MUST(DOM::create_element(document, HTML::TagNames::html, Namespace::HTML));
    auto head_element = MUST(DOM::create_element(document, HTML::TagNames::head, Namespace::HTML));
    MUST(html_node->append_child(head_element));
    auto body_element = MUST(DOM::create_element(document, HTML::TagNames::body, Namespace::HTML));
    MUST(html_node->append_child(body_element));
    MUST(document->append_child(html_node));
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#creating-a-new-browsing-context
BrowsingContext::BrowsingContextAndDocument BrowsingContext::create_a_new_browsing_context_and_document(GC::Ref<Page> page, GC::Ptr<DOM::Document> creator, GC::Ptr<DOM::Element> embedder, GC::Ptr<WindowProxy> existing_window_proxy, Optional<URL::Origin> determined_origin)
{
    // 1. Let browsingContext be a new browsing context.
    GC::Ref<BrowsingContext> browsing_context = *GC::Heap::the().allocate<BrowsingContext>(page);

    // 2. Let unsafeContextCreationTime be the unsafe shared current time.
    [[maybe_unused]] auto unsafe_context_creation_time = HighResolutionTime::unsafe_shared_current_time();

    // 3. Let creatorOrigin be null.
    Optional<URL::Origin> creator_origin = {};

    // 4. Let creatorBaseURL be null.
    Optional<URL::URL> creator_base_url = {};

    // 5. If creator is non-null, then:
    if (creator) {
        // 1. Set creatorOrigin to creator's origin.
        creator_origin = creator->origin();

        // 2. Set creatorBaseURL to creator's document base URL.
        creator_base_url = creator->base_url();

        // 3. Set browsingContext's virtual browsing context group ID to creator's browsing context's top-level browsing context's virtual browsing context group ID.
        // NB: The UI process holds this on the canonical browsing context.
    }

    // 6. Let sandboxFlags be the result of determining the creation sandboxing flags given browsingContext and embedder.
    auto sandbox_flags = determine_the_creation_sandboxing_flags(*browsing_context, embedder);

    // 7. Let origin be the result of determining the origin given about:blank, sandboxFlags, and creatorOrigin.
    // NB: The UI process determined the origin of a document it held first.
    auto origin = determined_origin.has_value() ? determined_origin.release_value() : determine_the_origin(URL::about_blank(), sandbox_flags, creator_origin);

    // FIXME: 8. Let permissionsPolicy be the result of creating a permissions policy given embedder and origin. [PERMISSIONSPOLICY]

    // 9. Let agent be the result of obtaining a similar-origin window agent given origin, group, and false.
    // NB: The UI process obtains the agent. For creator's origin, that is creator's agent. Any other origin is a new
    //     opaque one, whose agent cluster nothing else can reach.
    Optional<u64> agent_cluster_id;
    if (creator && origin.is_same_origin(*creator_origin))
        agent_cluster_id = creator->relevant_settings_object().agent_cluster_id();

    GC::Ptr<Window> window;

    // 10. Let realm execution context be the result of creating a new JavaScript realm given agent and the following customizations:
    auto realm_execution_context = Bindings::create_a_new_javascript_realm(
        Bindings::main_thread_vm(),
        [&](JS::Realm& realm) -> GC::Ref<JS::Object> {
            // NB: A container whose content navigable's document comes back from another process keeps the WindowProxy
            //     scripts hold for it.
            auto window_proxy = existing_window_proxy ? GC::Ref { *existing_window_proxy } : WindowProxy::create(realm);
            browsing_context->set_window_proxy(window_proxy);

            // - For the global object, create a new Window object.
            window = Window::create();
            return Bindings::create_global_object_wrapper(realm, GC::Ref { *window });
        },
        [&](JS::Realm&) -> GC::Ref<JS::Object> {
            // - For the global this binding, use browsingContext's WindowProxy object.
            return *browsing_context->window_proxy();
        });

    auto& realm = *realm_execution_context->realm;

    // 11. Let topLevelCreationURL be about:blank if embedder is null; otherwise embedder's relevant settings object's top-level creation URL.
    auto top_level_creation_url = !embedder ? URL::about_blank() : relevant_settings_object(*embedder).top_level_creation_url.value();

    // 12. Let topLevelOrigin be origin if embedder is null; otherwise embedder's relevant settings object's top-level origin.
    auto top_level_origin = !embedder ? origin : relevant_settings_object(*embedder).top_level_origin.value();

    // 13. Set up a window environment settings object with about:blank, realm execution context, null, topLevelCreationURL, and topLevelOrigin.
    WindowEnvironmentSettingsObject::setup(
        page,
        URL::about_blank(),
        move(realm_execution_context),
        {},
        top_level_creation_url,
        top_level_origin,
        agent_cluster_id);

    // 14. Let loadTimingInfo be a new document load timing info with its navigation start time set to the result of calling
    //     coarsen time with unsafeContextCreationTime and the new environment settings object's cross-origin isolated capability.
    auto load_timing_info = DOM::DocumentLoadTimingInfo();
    load_timing_info.navigation_start_time = HighResolutionTime::coarsen_time(
        unsafe_context_creation_time,
        as<WindowEnvironmentSettingsObject>(Bindings::principal_host_defined_environment_settings_object(realm)).cross_origin_isolated_capability());

    // 15. Let document be a new Document, with:
    auto document = HTML::HTMLDocument::create(page, *window);

    // Non-standard
    window->set_associated_document(*document);
    document->set_window(*window);

    // type: "html"
    document->set_document_type(DOM::Document::Type::HTML);

    // content type: "text/html"
    document->set_content_type("text/html"_utf16_fly_string);

    // mode: "quirks"
    document->set_quirks_mode(DOM::QuirksMode::Yes);

    // origin: origin
    document->set_origin(origin);

    // browsing context: browsingContext
    document->set_browsing_context(browsing_context);

    // FIXME: permissions policy: permissionsPolicy

    // active sandboxing flag set: sandboxFlags
    document->set_active_sandboxing_flag_set(sandbox_flags);

    // load timing info: loadTimingInfo
    document->set_load_timing_info(load_timing_info);

    // is initial about:blank: true
    document->set_is_initial_about_blank(true);
    // Spec issue: https://github.com/whatwg/html/issues/10261
    document->set_ready_to_run_scripts();

    // about base URL: creatorBaseURL
    document->set_about_base_url(creator_base_url);

    // allow declarative shadow roots: true
    document->set_allow_declarative_shadow_roots(HTML::HTMLParser::AllowDeclarativeShadowRoots::Yes);

    // custom element registry: A new CustomElementRegistry object.
    document->set_custom_element_registry(CustomElementRegistry::create_global(*document));

    // 16. Let iframeReferrerPolicy be the result of determining the iframe element referrer policy given embedder.
    auto iframe_referrer_policy = determine_iframe_element_referrer_policy(embedder);

    // 17. Set document's internal ancestor origin objects list to the result of running the internal ancestor origin
    //     objects list creation steps given document and iframeReferrerPolicy.
    document->set_internal_ancestor_origin_objects_list(document->internal_ancestor_origin_objects_list_creation_steps(iframe_referrer_policy));

    // 18. Set document's ancestor origins list to the result of running the ancestor origins list creation steps given document.
    document->set_ancestor_origins_list(document->ancestor_origins_list_creation_steps());

    // 19. If creator is non-null:
    if (creator) {
        // 1. Set document's referrer to the serialization of creator's URL.
        document->set_referrer(utf16_string_from_url_ascii(creator->url().serialize()));

        // 2. Set document's policy container to a clone of creator's policy container.
        document->set_policy_container(creator->policy_container()->clone(document->heap()));

        // 3. If creator's origin is same origin with creator's relevant settings object's top-level origin,
        if (creator->origin().is_same_origin(creator->relevant_settings_object().top_level_origin.value())) {
            // then set document's opener policy to creator's browsing context's top-level browsing context's active document's opener policy.
            // NB: That document is the top-level traversable's active document, which the traversable answers for
            //     a document hosted in another process.
            VERIFY(creator->navigable());
            document->set_opener_policy(creator->navigable()->top_level_traversable()->active_document_opener_policy());
        }
    }

    // 20. Assert: document's URL and document's relevant settings object's creation URL are about:blank.
    VERIFY(document->url() == URL::about_blank());
    VERIFY(document->relevant_settings_object().creation_url == URL::about_blank());

    // 21. Mark document as ready for post-load tasks.
    document->set_ready_for_post_load_tasks(true);

    // 22. Populate with html/head/body given document.
    populate_with_html_head_body(*document);
    if (!embedder)
        document->set_supported_color_schemes({ "light"_utf16_fly_string, "dark"_utf16_fly_string });

    // 23. Make active document.
    document->make_active();

    // 24. Completely finish loading document.
    document->completely_finish_loading();

    // 25. Return browsingContext and document.
    return BrowsingContext::BrowsingContextAndDocument { browsing_context, document };
}

BrowsingContext::BrowsingContext(GC::Ref<Page> page)
    : m_page(page)
{
}

BrowsingContext::~BrowsingContext() = default;

Optional<CrossProcessId> BrowsingContext::opener_navigable_id() const
{
    if (auto navigable = opener_navigable())
        return navigable->id();
    return {};
}

GC::Ptr<Navigable> BrowsingContext::opener_navigable() const
{
    if (!m_opener_browsing_context_window_proxy)
        return nullptr;
    return m_opener_browsing_context_window_proxy->navigable();
}

void BrowsingContext::set_opener_browsing_context(GC::Ptr<BrowsingContext> opener)
{
    m_opener_browsing_context_window_proxy = opener ? opener->window_proxy() : nullptr;
}

// NB: The browsing context active in a navigable another process hosts is there, and its WindowProxy stands for it.
void BrowsingContext::set_opener_browsing_context(Navigable& navigable)
{
    if (auto* remote_navigable = as_if<RemoteNavigable>(navigable))
        m_opener_browsing_context_window_proxy = remote_navigable->active_window_proxy_in_realm_of(*m_window_proxy->window());
    else
        m_opener_browsing_context_window_proxy = navigable.active_window_proxy();
}

void BrowsingContext::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);

    visitor.visit(m_page);
    visitor.visit(m_window_proxy);
    visitor.visit(m_active_document);
    visitor.visit(m_opener_browsing_context_window_proxy);
}

// https://html.spec.whatwg.org/multipage/browsers.html#top-level-browsing-context
bool BrowsingContext::is_top_level() const
{
    // A top-level browsing context is a browsing context whose active document's node navigable is a traversable navigable.
    return active_document() != nullptr && active_document()->navigable() != nullptr && active_document()->navigable()->is_traversable();
}

GC::Ptr<BrowsingContext> BrowsingContext::top_level_browsing_context() const
{
    auto const* start = this;

    // 1. If start's active document is not fully active, then return null.
    if (!start->active_document()->is_fully_active()) {
        return nullptr;
    }

    // 2. Let navigable be start's active document's node navigable.
    GC::Ptr<Navigable> navigable = start->active_document()->navigable();

    // 3. While navigable's parent is not null, set navigable to navigable's parent.
    while (navigable->parent())
        navigable = navigable->parent();

    // 4. Return navigable's active browsing context.
    // NB: This is null if another process hosts navigable's document.
    auto* local_navigable = as_if<LocalNavigable>(*navigable);
    if (!local_navigable)
        return nullptr;
    return local_navigable->active_browsing_context();
}

// https://html.spec.whatwg.org/multipage/browsers.html#active-document
DOM::Document const* BrowsingContext::active_document() const
{
    return m_active_document.ptr();
}

// https://html.spec.whatwg.org/multipage/browsers.html#active-document
DOM::Document* BrowsingContext::active_document()
{
    return m_active_document.ptr();
}

void BrowsingContext::set_active_document(GC::Ptr<DOM::Document> document)
{
    m_active_document = document;
}

// https://html.spec.whatwg.org/multipage/browsers.html#active-window
HTML::Window* BrowsingContext::active_window()
{
    // A browsing context's active window is its WindowProxy object's [[Window]] internal slot value.
    return m_window_proxy->window().ptr();
}

// https://html.spec.whatwg.org/multipage/browsers.html#active-window
HTML::Window const* BrowsingContext::active_window() const
{
    // A browsing context's active window is its WindowProxy object's [[Window]] internal slot value.
    return m_window_proxy->window().ptr();
}

HTML::WindowProxy* BrowsingContext::window_proxy()
{
    return m_window_proxy.ptr();
}

HTML::WindowProxy const* BrowsingContext::window_proxy() const
{
    return m_window_proxy.ptr();
}

HTML::WindowProxy* BrowsingContext::window_proxy_for(Bindings::WrapperWorld& wrapper_world, JS::Realm& realm)
{
    if (wrapper_world.is_main_world()) {
        VERIFY(m_window_proxy);
        return window_proxy();
    }

    auto& cache = m_window_proxies.cache_for(wrapper_world);
    if (auto proxy = cache.get(wrapper_world))
        return proxy.ptr();

    auto proxy = WindowProxy::create(realm);
    if (auto window = active_window())
        proxy->set_window(*window);
    cache.set(wrapper_world, proxy);
    return proxy.ptr();
}

void BrowsingContext::set_active_window(GC::Ref<HTML::Window> window)
{
    m_window_proxy->set_window(window);
    m_window_proxies.for_each([&](auto& cache) {
        cache.for_each([&](auto& proxy) {
            proxy.set_window(window);
        });
    });
}

void BrowsingContext::set_window_proxy(GC::Ptr<WindowProxy> window_proxy)
{
    m_window_proxy = move(window_proxy);
}

// https://html.spec.whatwg.org/multipage/origin.html#one-permitted-sandboxed-navigator
BrowsingContext const* BrowsingContext::the_one_permitted_sandboxed_navigator() const
{
    // FIXME: Implement this.
    return nullptr;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#ancestor-browsing-context
bool BrowsingContext::is_ancestor_of(BrowsingContext const& potential_descendant) const
{
    // A browsing context potentialDescendant is said to be an ancestor of a browsing context potentialAncestor if the following algorithm returns true:

    // 1. Let potentialDescendantDocument be potentialDescendant's active document.
    auto const* potential_descendant_document = potential_descendant.active_document();

    // 2. If potentialDescendantDocument is not fully active, then return false.
    if (!potential_descendant_document->is_fully_active())
        return false;

    // 3. Let ancestorBCs be the list obtained by taking the browsing context of the active document of each member of potentialDescendantDocument's ancestor navigables.
    // NB: The browsing context of an ancestor hosted by another process is there, and is not potentialAncestor.
    for (auto const& ancestor : potential_descendant_document->ancestor_navigables()) {
        auto* local_ancestor = as_if<HTML::LocalNavigable>(*ancestor);

        // 4. If ancestorBCs contains potentialAncestor, then return true.
        if (local_ancestor && local_ancestor->active_browsing_context().ptr() == this)
            return true;
    }

    // 5. Return false.
    return false;
}

// https://html.spec.whatwg.org/multipage/browsers.html#determining-the-creation-sandboxing-flags
SandboxingFlagSet determine_the_creation_sandboxing_flags(BrowsingContext const& browsing_context, GC::Ptr<DOM::Element> embedder)
{
    // To determine the creation sandboxing flags for a browsing context browsing context, given null or an element
    // embedder, return the union of the flags that are present in the following sandboxing flag sets:
    SandboxingFlagSet sandboxing_flags {};

    // - If embedder is null, then: the flags set on browsing context's popup sandboxing flag set.
    if (!embedder) {
        sandboxing_flags |= browsing_context.popup_sandboxing_flag_set();
    } else {
        // - If embedder is an element, then: the flags set on embedder's iframe sandboxing flag set.
        if (is<HTMLIFrameElement>(embedder.ptr())) {
            auto const& iframe_element = static_cast<HTMLIFrameElement const&>(*embedder);
            sandboxing_flags |= iframe_element.iframe_sandboxing_flag_set();
        }

        // - If embedder is an element, then: the flags set on embedder's node document's active sandboxing flag set.
        sandboxing_flags |= embedder->document().active_sandboxing_flag_set();
    }

    return sandboxing_flags;
}

// https://html.spec.whatwg.org/multipage/browsers.html#determining-the-creation-sandboxing-flags
// Given the navigable whose container is embedder, which reads the container's facts wherever the element is.
SandboxingFlagSet determine_the_creation_sandboxing_flags(BrowsingContext const& browsing_context, Navigable const& navigable)
{
    // To determine the creation sandboxing flags for a browsing context browsing context, given null or an element
    // embedder, return the union of the flags that are present in the following sandboxing flag sets:
    SandboxingFlagSet sandboxing_flags {};

    // - If embedder is null, then: the flags set on browsing context's popup sandboxing flag set.
    if (!navigable.container_local_name().has_value()) {
        sandboxing_flags |= browsing_context.popup_sandboxing_flag_set();
    } else {
        // - If embedder is an element, then: the flags set on embedder's iframe sandboxing flag set.
        sandboxing_flags |= navigable.container_iframe_sandboxing_flag_set();

        // - If embedder is an element, then: the flags set on embedder's node document's active sandboxing flag set.
        sandboxing_flags |= navigable.container_document_active_sandboxing_flag_set();
    }

    return sandboxing_flags;
}

bool BrowsingContext::has_navigable_been_destroyed() const
{
    auto const* document = active_document();
    if (!document)
        return true;
    auto navigable = document->navigable();
    return !navigable || navigable->has_been_destroyed();
}

}
