/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/CanonicalNavigable.h>

#include <AK/AnyOf.h>
#include <AK/Random.h>
#include <LibCore/StatsLog.h>
#include <LibHTTP/HeaderList.h>
#include <LibWebCommon/HTML/BrowsingContext.h>
#include <LibWebCommon/HTML/HistoryOperation.h>
#include <LibWebCommon/HTML/SerializationRecords.h>
#include <LibWebCommon/Page/ViewportIsFullscreen.h>
#include <LibWebCommon/WebView/SiteIsolation.h>
#include <LibWebView/Application.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/CanonicalBrowsingContext.h>
#include <LibWebView/CanonicalBrowsingContextGroup.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/CanonicalEnvironmentSettingsObject.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/CanonicalWindow.h>
#include <LibWebView/PictureInPictureManager.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentClient.h>

namespace WebView {

CanonicalNavigable::CanonicalNavigable(Web::HTML::CrossProcessId id)
    : m_id(id)
{
}

RefPtr<WebContentPage> CanonicalNavigable::reporting_page() const
{
    if (!m_container_document)
        return {};
    return m_container_document->host();
}

CanonicalDocument& CanonicalNavigable::active_document() const
{
    // A navigable's active document is its active session history entry's document.
    VERIFY(m_active_session_history_entry && m_active_session_history_entry->document_state->document);
    return *m_active_session_history_entry->document_state->document;
}

URL::URL const& CanonicalNavigable::active_document_url() const
{
    VERIFY(m_active_session_history_entry);
    return m_active_session_history_entry->url;
}

CanonicalBrowsingContext& CanonicalNavigable::active_browsing_context() const
{
    // A navigable's active browsing context is its active document's browsing context.
    return active_document().browsing_context();
}

static Utf16String generate_a_random_uuid()
{
    auto uuid = generate_random_uuid();
    return Utf16String::from_ascii_without_validation(uuid.bytes());
}

RefPtr<WebContentPage> CanonicalNavigation::worker() const
{
    return state.visit([&](auto const& state) { return state.runs_in == RunsIn::Worker ? page : nullptr; });
}

RefPtr<WebContentPage> CanonicalNavigation::host() const
{
    return state.visit([&](auto const& state) { return state.runs_in == RunsIn::Host ? page : nullptr; });
}

RefPtr<NavigationLoader> CanonicalNavigation::loader() const
{
    return state.visit([](auto const& state) -> RefPtr<NavigationLoader> {
        if constexpr (requires { state.loader; })
            return state.loader;
        else
            return nullptr;
    });
}

// A navigation made again from the same source — one the user stopped, or a crash cut short — as a GET: A retry
// shouldn't resubmit a form behind the user's back, so the document resource isn't sent again.
Web::HTML::PreparedNavigationDescriptor prepare_navigation_to_retry(Web::HTML::PreparedNavigationDescriptor navigation)
{
    navigation.document_resource = {};
    navigation.navigation_api_state = {};
    navigation.csp_navigation_type = Web::ContentSecurityPolicy::Directives::NavigationType::Other;
    navigation.navigation_id = generate_a_random_uuid();
    return navigation;
}

Web::HTML::PreparedNavigationDescriptor prepare_navigation_to_retry(Web::HTML::NavigationStartRequest const& request)
{
    return prepare_navigation_to_retry({
        .url = request.url,
        .document_resource = {},
        .history_handling = request.history_handling,
        .navigation_api_state = {},
        .referrer_policy = request.request_referrer_policy,
        .user_involvement = request.user_involvement,
        .navigation_id = {},
        .initial_insertion = Web::HTML::InitialInsertion::No,
        .csp_navigation_type = request.csp_navigation_type,
        .source_snapshot_params = request.source_snapshot_params,
        .initiator_origin_snapshot = request.initiator_origin,
        .initiator_base_url_snapshot = request.initiator_base_url.value_or(URL::about_blank()),
    });
}

static Web::HTML::HistoryHandlingBehavior to_history_handling_behavior(Web::Bindings::NavigationHistoryBehavior history_handling)
{
    VERIFY(history_handling != Web::Bindings::NavigationHistoryBehavior::Auto);
    return history_handling == Web::Bindings::NavigationHistoryBehavior::Push ? Web::HTML::HistoryHandlingBehavior::Push : Web::HTML::HistoryHandlingBehavior::Replace;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#the-navigation-must-be-a-replace
static bool navigation_must_be_a_replace(URL::URL const& url, CanonicalDocument const& document)
{
    return url.scheme() == "javascript"sv || document.is_initial_about_blank();
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#navigate
// NB: This is a navigation from the browser's UI, whose sourceDocument is null.
void CanonicalNavigable::navigate(URL::URL url, Web::HTML::DocumentResource document_resource, Web::Bindings::NavigationHistoryBehavior history_handling)
{
    begin_navigation(prepare_navigation(move(url), move(document_resource), history_handling));
}

// Steps 1 to 7 of navigate, for a navigation from the browser's UI.
Web::HTML::PreparedNavigationDescriptor CanonicalNavigable::prepare_navigation(URL::URL url, Web::HTML::DocumentResource document_resource, Web::Bindings::NavigationHistoryBehavior history_handling)
{
    auto user_involvement = Web::HTML::UserNavigationInvolvement::BrowserUI;

    // 1. Let cspNavigationType be "form-submission" if formDataEntryList is non-null; otherwise "other".
    auto csp_navigation_type = Web::ContentSecurityPolicy::Directives::NavigationType::Other;

    // 2. Let sourceSnapshotParams be the result of snapshotting source snapshot params given sourceDocument.
    auto source_snapshot_params = Web::HTML::create_navigation_source_snapshot_without_a_source_document();

    // 3. Let initiatorOriginSnapshot be a new opaque origin.
    auto initiator_origin_snapshot = URL::Origin::create_opaque();

    // 4. Let initiatorBaseURLSnapshot be about:blank.
    auto initiator_base_url_snapshot = URL::about_blank();

    // 5. If sourceDocument is null:
    //    1. Assert: userInvolvement is "browser UI".
    //    2. If url's scheme is "javascript", then set initiatorOriginSnapshot to navigable's active document's origin.
    if (url.scheme() == "javascript"sv)
        initiator_origin_snapshot = active_document().origin();

    // 7. Let navigationId be the result of generating a random UUID.
    auto navigation_id = generate_a_random_uuid();

    // 8. If the surrounding agent is equal to navigable's active document's relevant agent, then continue these steps.
    //    Otherwise, queue a global task on the navigation and traversal task source given navigable's active window to
    //    continue these steps.
    // NB: The remaining steps run here, but for those that need navigable's active document, which run in the process
    //     hosting it.
    return {
        .url = move(url),
        .document_resource = move(document_resource),
        .history_handling = history_handling,
        .navigation_api_state = {},
        .referrer_policy = Web::ReferrerPolicy::ReferrerPolicy::EmptyString,
        .user_involvement = user_involvement,
        .navigation_id = move(navigation_id),
        .initial_insertion = Web::HTML::InitialInsertion::No,
        .csp_navigation_type = csp_navigation_type,
        .source_snapshot_params = move(source_snapshot_params),
        .initiator_origin_snapshot = move(initiator_origin_snapshot),
        .initiator_base_url_snapshot = move(initiator_base_url_snapshot),
    };
}

// Continue the navigate algorithm at step 9 with the values prepared by steps 1-7.
void CanonicalNavigable::begin_navigation(Web::HTML::PreparedNavigationDescriptor navigation)
{
    auto& traversable = top_level_traversable();
    auto const& url = navigation.url;
    auto user_involvement = navigation.user_involvement;
    auto const& navigation_id = navigation.navigation_id;

    // 9. If navigable's active document's unload counter is greater than 0, then invoke WebDriver BiDi navigation failed
    //    with navigable and a WebDriver BiDi navigation status whose id is navigationId, status is "canceled", and url
    //    is url, and return.
    if (active_document().unload_counter() > 0)
        return;

    // NB: The page hosting navigable's active document, which has its container if any process does, runs steps 10,
    //     11, and 15.

    // 12. If historyHandling is "auto", then:
    auto history_handling = navigation.history_handling;
    if (history_handling == Web::Bindings::NavigationHistoryBehavior::Auto) {
        // 1. If url equals navigable's active document's URL, and either userInvolvement is "browser UI" or
        //    initiatorOriginSnapshot is same origin with navigable's active document's origin, then set historyHandling
        //    to "replace".
        if (url == active_document_url()
            && (user_involvement == Web::HTML::UserNavigationInvolvement::BrowserUI || navigation.initiator_origin_snapshot.is_same_origin(active_document().origin()))) {
            history_handling = Web::Bindings::NavigationHistoryBehavior::Replace;
        }

        // 2. Otherwise, set historyHandling to "push".
        else {
            history_handling = Web::Bindings::NavigationHistoryBehavior::Push;
        }
    }

    // 13. If the navigation must be a replace given url and navigable's active document, then set historyHandling to
    //     "replace".
    if (navigation_must_be_a_replace(url, active_document()))
        history_handling = Web::Bindings::NavigationHistoryBehavior::Replace;

    // 14. If all of the following are true:
    //     - documentResource is null;
    //     - response is null;
    //     - url equals navigable's active session history entry's URL with exclude fragments set to true; and
    //     - url's fragment is non-null,
    //     then:
    // NB: A lost document is not there to navigate to a fragment of: it is loaded again instead.
    auto host = active_document().host();
    if (host
        && navigation.document_resource.has<Empty>()
        && url.equals(active_session_history_entry()->url, URL::ExcludeFragment::Yes)
        && url.fragment().has_value()) {
        // 1. Navigate to a fragment given navigable, url, historyHandling, userInvolvement, sourceElement,
        //    navigationAPIState, and navigationId.
        host->async_navigate_to_a_fragment(id(), url, to_history_handling_behavior(history_handling), user_involvement, navigation_id);

        // 2. Return.
        return;
    }

    // 16. Let targetSnapshotParams be the result of snapshotting target snapshot params given navigable.
    auto target_snapshot_params = snapshot_target_snapshot_params();

    // FIXME: 17. Invoke WebDriver BiDi navigation started with navigable and a new WebDriver BiDi navigation status whose
    //            id is navigationId, status is "pending", and url is url.

    // 18. If navigable's ongoing navigation is "traversal", then:
    // AD-HOC: A navigation from the browser's UI goes on, and takes the navigable over from the traversal.
    if (m_ongoing_navigation.has<Traversal>() && user_involvement != Web::HTML::UserNavigationInvolvement::BrowserUI) {
        // FIXME: 1. Invoke WebDriver BiDi navigation failed with navigable and a new WebDriver BiDi navigation status
        //           whose id is navigationId, status is "canceled", and url is url.

        // 2. Return.
        return;
    }

    // NB: The process hosting navigable's active document evaluates a javascript: URL at step 20. A lost document is
    //     not there to evaluate it against, and the navigation does nothing.
    auto is_javascript_url = url.scheme() == "javascript"sv;
    if (is_javascript_url && !host)
        return;

    // 19. Set the ongoing navigation for navigable to navigationId.
    // NB: It is set with the navigation's steps: evaluating a javascript: URL from step 20, or those step 23 runs.

    // 20. If url's scheme is "javascript", then:
    if (is_javascript_url) {
        begin_evaluating_javascript_url(url, navigation_id, *host);

        // 1. Let request be a new request whose URL is url and whose policy container is sourceSnapshotParams's source
        //    policy container.
        // 2. Queue a global task on the navigation and traversal task source given navigable's active window to
        //    navigate to a javascript: URL given navigable, request, historyHandling, initiatorOriginSnapshot,
        //    userInvolvement, cspNavigationType, initialInsertion, and navigationId.
        host->async_navigate_to_a_javascript_url(id(), url, to_history_handling_behavior(history_handling), navigation.initiator_origin_snapshot, navigation.source_snapshot_params, user_involvement, navigation.csp_navigation_type, navigation_id);

        // 3. Return.
        return;
    }

    // NB: Step 21 fires the navigate event at navigable's active window, which is gone with a lost document. It is not
    //     fired for a navigation from the browser's UI, nor for a sourceDocument in another agent, which is never same
    //     origin-domain with navigable's active document.

    // FIXME: 22. If sourceDocument is navigable's container document, then reserve deferred fetch quota for navigable's
    //            container given url's origin.

    // 23. In parallel, run these steps:
    // NB: The page hosting navigable's active document, or standing in for it, runs the unload check and population's
    //     first steps.
    auto worker = traversable.page_hosting(*this);
    if (!worker || !worker->is_open()) {
        clear_ongoing_navigation();
        return;
    }
    auto start_request = Web::HTML::NavigationStartRequest {
        .navigable_id = id(),
        .url = url,
        .document_resource = move(navigation.document_resource),
        .request_referrer = Web::Fetch::Infrastructure::RequestReferrer::Client,
        .request_referrer_policy = navigation.referrer_policy,
        .initiator_origin = navigation.initiator_origin_snapshot,
        .initiator_base_url = navigation.initiator_base_url_snapshot,
        .navigable_target_name = active_session_history_entry()->document_state->navigable_target_name,
        .source_snapshot_params = move(navigation.source_snapshot_params),
        .target_snapshot_params = target_snapshot_params,
        .csp_navigation_type = navigation.csp_navigation_type,
        .history_handling = history_handling,
        .user_involvement = user_involvement,
        .navigation_id = navigation_id,
        .classic_history_api_state = Web::HTML::storage_serialization_record_for_null(),
        .navigation_api_state = Web::HTML::storage_serialization_record_for_undefined(),
        .navigation_api_key = generate_a_random_uuid(),
        .navigation_api_id = generate_a_random_uuid(),
        .directive_state_id = {},
        .directive_state_value = {},
    };
    auto retry = prepare_navigation_to_retry(start_request);
    set_ongoing_navigation({
        .url = url,
        .navigation_id = navigation_id,
        .retry = move(retry),
        .sequence_number = traversable.next_sequence_number(),
        .state = CanonicalNavigation::CheckingIfUnloadingIsCanceled { move(start_request) },
        .page = worker,
    });
    worker->async_set_ongoing_navigation(id(), navigation_id);
    worker->begin_navigation_unload_check(*this, navigation_id);
}

// https://html.spec.whatwg.org/multipage/browsers.html#obtain-browsing-context-navigation
// NB: The browsing context is returned with its active document, which nothing else holds for a new one.
CanonicalBrowsingContext::BrowsingContextAndDocument CanonicalNavigable::obtain_a_browsing_context_to_use_for_a_navigation_response(NavigationLoader::ResponseDocument const& navigation_params)
{
    // 1. Let browsingContext be navigationParams's navigable's active browsing context.
    CanonicalBrowsingContext::BrowsingContextAndDocument browsing_context { active_browsing_context(), active_document() };

    // 2. If browsingContext is not a top-level browsing context, then return browsingContext.
    if (!is_top_level_traversable())
        return browsing_context;

    // 3. Let coopEnforcementResult be navigationParams's COOP enforcement result.
    auto const& coop_enforcement_result = navigation_params.coop_enforcement_result;

    // 4. Let swapGroup be coopEnforcementResult's needs a browsing context group switch.
    auto swap_group = coop_enforcement_result.needs_a_browsing_context_group_switch;

    // NB: Steps 5-8 only affect swapGroup through optional choices. This implementation does not take them.

    // 9. If swapGroup is false, then:
    if (!swap_group) {
        // FIXME: 1. If coopEnforcementResult's would need a browsing context group switch due to report-only is true,
        //           set browsingContext's virtual browsing context group ID to a new unique identifier.

        // 2. Return browsingContext.
        return browsing_context;
    }

    // 10. Let newBrowsingContext be the first return value of creating a new top-level browsing context and document.
    // NB: The navigation response's document replaces that document before any process creates it.
    auto new_browsing_context = CanonicalBrowsingContext::create_a_new_top_level_browsing_context_and_document({});

    // 11. Let navigationCOOP be navigationParams's cross-origin opener policy.
    auto const& navigation_coop = navigation_params.opener_policy;

    // 12. If navigationCOOP's value is "same-origin-plus-COEP", then set newBrowsingContext's group's cross-origin
    //     isolation mode to either "logical" or "concrete". The choice of which is implementation-defined.
    if (navigation_coop.value == Web::HTML::OpenerPolicyValue::SameOriginPlusCOEP)
        new_browsing_context.browsing_context->group()->set_cross_origin_isolation_mode(Web::HTML::CrossOriginIsolationMode::Concrete);

    // 13. Let sandboxFlags be a clone of navigationParams's final sandboxing flag set.
    // FIXME: 14. If sandboxFlags is not empty, then:
    //            1. Assert: navigationCOOP's value is "unsafe-none".
    //            2. Assert: newBrowsingContext's popup sandboxing flag set is empty.
    //            3. Set newBrowsingContext's popup sandboxing flag set to sandboxFlags.

    // 15. Return newBrowsingContext.
    return new_browsing_context;
}

// https://html.spec.whatwg.org/multipage/document-lifecycle.html#initialise-the-document-object
NonnullRefPtr<CanonicalDocument> CanonicalNavigable::create_and_initialize_a_document(NavigationLoader::ResponseDocument const& navigation_params)
{
    // 1. Let browsingContext be the result of obtaining a browsing context to use for a navigation response given navigationParams.
    auto browsing_context_and_document = obtain_a_browsing_context_to_use_for_a_navigation_response(navigation_params);
    auto& browsing_context = browsing_context_and_document.browsing_context;

    // 2. Let permissionsPolicy be the result of creating a permissions policy from a response given navigationParams's
    //    navigable's container, navigationParams's origin, and navigationParams's response.
    // NB: Navigation params the UI process handled carry the one it created from the response's headers. Any other
    //     response, such as a javascript: URL's or inline content's, has no Permissions-Policy header.
    auto permissions_policy = navigation_params.permissions_policy;
    if (!permissions_policy.has_value())
        permissions_policy = create_a_permissions_policy_from_a_response(navigation_params.origin, HTTP::HeaderList::create());

    // 3. Let creationURL be navigationParams's response's URL.
    auto creation_url = navigation_params.response_url;

    // 4. If navigationParams's request is non-null, then set creationURL to navigationParams's request's current URL.
    if (navigation_params.request_current_url.has_value())
        creation_url = *navigation_params.request_current_url;

    // 5. Let window be null.
    RefPtr<CanonicalWindow> window;

    // 6. If browsingContext's active document's is initial about:blank is true, and browsingContext's active document's
    //    origin is same origin-domain with navigationParams's origin, then set window to browsingContext's active window.
    if (browsing_context->active_document()->is_initial_about_blank()
        && browsing_context->active_document()->origin().is_same_origin_domain(navigation_params.origin)) {
        window = browsing_context->active_window();
    }
    // 7. Otherwise:
    else {
        // FIXME: 1. Let oacHeader be the result of getting a structured field value given `Origin-Agent-Cluster` and "item"
        //           from navigationParams's response's header list.
        // FIXME: 2. Let requestsOAC be true if oacHeader is not null and oacHeader[0] is the boolean true; otherwise false.
        // FIXME: 3. If navigationParams's reserved environment is a non-secure context, then set requestsOAC to false.
        auto requests_oac = false;

        // 4. Let agent be the result of obtaining a similar-origin window agent given navigationParams's origin,
        //    browsingContext's group, and requestsOAC.
        // AD-HOC: Only a top-level browsing context has a group. A child browsing context's is its top-level browsing
        //         context's.
        auto agent = browsing_context->top_level_browsing_context().group()->obtain_similar_origin_window_agent(navigation_params.origin, requests_oac);

        // 5. Let realmExecutionContext be the result of creating a new realm given agent and the following customizations:
        //    - For the global object, create a new Window object.
        //    - For the global this binding, use browsingContext's WindowProxy object.
        // 6. Set window to the global object of realmExecutionContext's Realm component.
        // NB: The realm is in the process hosting agent.
        window = CanonicalWindow::create(agent);

        // 7. Let topLevelCreationURL be creationURL.
        auto top_level_creation_url = creation_url;

        // 8. Let topLevelOrigin be navigationParams's origin.
        // NB: The UI process derives the top-level origin from the window's associated document.

        // 9. If navigable's container is not null, then:
        if (m_parent) {
            // 1. Let parentEnvironment be navigable's container's relevant settings object.
            auto const& parent_environment = m_parent->active_document().relevant_global_object().relevant_settings_object();

            // 2. Set topLevelCreationURL to parentEnvironment's top-level creation URL.
            top_level_creation_url = parent_environment.top_level_creation_url().value();

            // 3. Set topLevelOrigin to parentEnvironment's top-level origin.
        }

        // 10. Set up a window environment settings object with creationURL, realmExecutionContext, navigationParams's
        //     reserved environment, topLevelCreationURL, and topLevelOrigin.
        window->set_up_a_window_environment_settings_object(navigation_params.environment_id, move(top_level_creation_url));
    }

    // 9. Let document be a new Document, with
    //    origin: navigationParams's origin
    //    browsing context: browsingContext
    //    policy container: navigationParams's policy container
    //    permissions policy: permissionsPolicy
    //    active sandboxing flag set: navigationParams's final sandboxing flag set
    //    opener policy: navigationParams's cross-origin opener policy
    //    URL: creationURL
    // NB: The process hosting window's agent creates the document, with its other fields, and runs the remaining steps.
    //     The UI process holds the embedder policy of the document's policy container.
    auto document = CanonicalDocument::create(navigation_params.document_id, move(creation_url), navigation_params.origin, browsing_context, window.release_nonnull(), navigation_params.final_sandboxing_flag_set, CanonicalDocument::IsInitialAboutBlank::No);
    document->set_opener_policy(navigation_params.opener_policy);
    document->set_embedder_policy(navigation_params.embedder_policy);
    document->set_permissions_policy(permissions_policy.release_value());

    // 22. Return document.
    return document;
}

CanonicalNavigable::~CanonicalNavigable()
{
    if (m_active_session_history_entry)
        m_active_session_history_entry->document_state->document = nullptr;
}

bool CanonicalNavigable::has_remote_host() const
{
    if (!m_parent || !m_active_session_history_entry || !active_document().host())
        return false;
    return active_document().host() != reporting_page();
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#nav-top
CanonicalTraversable& CanonicalNavigable::top_level_traversable()
{
    // 1. Let navigable be inputNavigable.
    auto* navigable = this;

    // 2. While navigable's parent is not null, set navigable to navigable's parent.
    while (navigable->parent())
        navigable = navigable->parent();

    // 3. Return navigable.
    VERIFY(navigable->is_top_level_traversable());
    return static_cast<CanonicalTraversable&>(*navigable);
}

CanonicalTraversable const& CanonicalNavigable::top_level_traversable() const
{
    return const_cast<CanonicalNavigable&>(*this).top_level_traversable();
}

void CanonicalNavigable::set_container_document(Badge<CanonicalTraversable>, CanonicalDocument& document)
{
    m_container_document = document;
}

CanonicalNavigable& CanonicalNavigable::append_child(NonnullOwnPtr<CanonicalNavigable> child)
{
    VERIFY(!child->m_parent);
    child->m_parent = this;
    m_children.append(move(child));
    return *m_children.last();
}

NonnullOwnPtr<CanonicalNavigable> CanonicalNavigable::remove_child(CanonicalNavigable& child)
{
    for (size_t i = 0; i < m_children.size(); ++i) {
        if (m_children[i].ptr() != &child)
            continue;

        auto removed_child = m_children.take(i);
        VERIFY(removed_child->m_parent == this);
        removed_child->m_parent = nullptr;
        return removed_child;
    }

    VERIFY_NOT_REACHED();
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#nav-target-name
Utf16String const& CanonicalNavigable::target_name() const
{
    // A navigable's target name is its active session history entry's document state's navigable target name.
    return active_session_history_entry()->document_state->navigable_target_name;
}

bool CanonicalNavigable::is_ancestor_of(CanonicalNavigable const& potential_descendant) const
{
    for (auto const* parent = potential_descendant.parent(); parent; parent = parent->parent()) {
        if (parent == this)
            return true;
    }
    return false;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#familiar-with
// NB: Stated on the navigables whose active browsing contexts are compared, as in the renderer.
bool CanonicalNavigable::is_familiar_with(CanonicalNavigable const& other) const
{
    // A browsing context A is familiar with a second browsing context B if the following algorithm returns true:
    auto const& A = *this;
    auto const& B = other;

    // 1. If A's active document's origin is same origin with B's active document's origin, then return true.
    if (A.active_document().origin().is_same_origin(B.active_document().origin()))
        return true;

    // 2. If A's top-level browsing context is B, then return true.
    if (&A.top_level_traversable() == &B)
        return true;

    // 3. If B is an auxiliary browsing context and A is familiar with B's opener browsing context, then return true.
    if (B.active_browsing_context().is_auxiliary()) {
        if (auto opener = B.active_browsing_context().opener_browsing_context()) {
            if (auto const* opener_navigable = CanonicalTraversable::navigable_with_active_browsing_context(*opener); opener_navigable && A.is_familiar_with(*opener_navigable))
                return true;
        }
    }

    // 4. If there exists an ancestor browsing context of B whose active document has the same origin as the active document of A, then return true.
    // NOTE: This includes the case where A is an ancestor browsing context of B.
    for (auto const* ancestor = B.parent(); ancestor; ancestor = ancestor->parent()) {
        if (ancestor->active_document().origin().is_same_origin(A.active_document().origin()))
            return true;
    }

    // 5. Return false.
    return false;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#allowed-to-navigate
bool CanonicalNavigable::allowed_by_sandboxing_to_navigate(CanonicalNavigable const& target, Web::InitiatorSourceSnapshot const& source_snapshot_params) const
{
    auto const& source = *this;

    // 1. If source is target, then return true.
    if (&source == &target)
        return true;

    // 2. If source is an ancestor of target, then return true.
    if (source.is_ancestor_of(target))
        return true;

    // 3. If target is an ancestor of source, then:
    if (target.is_ancestor_of(source)) {
        // 1. If target is not a top-level traversable, then return true.
        if (!target.is_top_level_traversable())
            return true;

        // 2. If sourceSnapshotParams's has transient activation is true, and sourceSnapshotParams's sandboxing flags's
        //    sandboxed top-level navigation with user activation browsing context flag is set, then return false.
        if (source_snapshot_params.has_transient_activation
            && has_flag(source_snapshot_params.sandboxing_flags, Web::HTML::SandboxingFlagSet::SandboxedTopLevelNavigationWithUserActivation)) {
            return false;
        }

        // 3. If sourceSnapshotParams's has transient activation is false, and sourceSnapshotParams's sandboxing flags's
        //    sandboxed top-level navigation without user activation browsing context flag is set, then return false.
        if (!source_snapshot_params.has_transient_activation
            && has_flag(source_snapshot_params.sandboxing_flags, Web::HTML::SandboxingFlagSet::SandboxedTopLevelNavigationWithoutUserActivation)) {
            return false;
        }

        // 4. Return true.
        return true;
    }

    // 4. If target is a top-level traversable:
    if (target.is_top_level_traversable()) {
        // FIXME: 1. If source is the one permitted sandboxed navigator of target, then return true.

        // 2. If sourceSnapshotParams's sandboxing flags's sandboxed navigation browsing context flag is set, then return false.
        if (has_flag(source_snapshot_params.sandboxing_flags, Web::HTML::SandboxingFlagSet::SandboxedNavigation))
            return false;

        // 3. Return true.
        return true;
    }

    // 5. If sourceSnapshotParams's sandboxing flags's sandboxed navigation browsing context flag is set, then return false.
    if (has_flag(source_snapshot_params.sandboxing_flags, Web::HTML::SandboxingFlagSet::SandboxedNavigation))
        return false;

    // 6. Return true.
    return true;
}

Web::PermissionsPolicy::PermissionsPolicy CanonicalNavigable::create_a_permissions_policy_from_a_response(URL::Origin const& origin, HTTP::HeaderList const& response_header_list) const
{
    if (!m_container_document)
        return Web::PermissionsPolicy::create_a_permissions_policy_for_a_navigable_from_response({}, origin, response_header_list);

    Web::PermissionsPolicy::PolicyDirective container_policy;
    if (m_hosted_state.has_value())
        container_policy = m_hosted_state->container.container_policy;
    Web::PermissionsPolicy::NavigableContainerPolicyInputs container { m_container_document->permissions_policy(), m_container_document->origin(), container_policy };
    return Web::PermissionsPolicy::create_a_permissions_policy_for_a_navigable_from_response(container, origin, response_header_list);
}

Web::HTML::TargetSnapshotParams CanonicalNavigable::snapshot_target_snapshot_params() const
{
    Optional<Web::HTML::ReplicatedContainerState const&> container;
    if (m_hosted_state.has_value() && m_hosted_state->container.local_name.has_value())
        container = m_hosted_state->container;

    // To snapshot target snapshot params given a navigable targetNavigable, return a new target snapshot params with:
    return {
        // sandboxing flags
        //     the result of determining the creation sandboxing flags given targetNavigable's active browsing context
        //     and targetNavigable's container
        .sandboxing_flags = determine_the_creation_sandboxing_flags(active_browsing_context(), container),

        // iframe element referrer policy
        //     the result of determining the iframe element referrer policy given targetNavigable's container
        .iframe_element_referrer_policy = container.has_value() ? container->iframe_referrer_policy : Web::ReferrerPolicy::ReferrerPolicy::EmptyString,
    };
}

IterationDecision CanonicalNavigable::for_each_in_inclusive_subtree(Function<IterationDecision(CanonicalNavigable&)> const& callback)
{
    if (callback(*this) == IterationDecision::Break)
        return IterationDecision::Break;

    return for_each_in_subtree(callback);
}

IterationDecision CanonicalNavigable::for_each_in_subtree(Function<IterationDecision(CanonicalNavigable&)> const& callback)
{
    for (auto const& child : m_children) {
        if (child->for_each_in_inclusive_subtree(callback) == IterationDecision::Break)
            return IterationDecision::Break;
    }

    return IterationDecision::Continue;
}

IterationDecision CanonicalNavigable::for_each_in_inclusive_subtree(Function<IterationDecision(CanonicalNavigable const&)> const& callback) const
{
    if (callback(*this) == IterationDecision::Break)
        return IterationDecision::Break;

    return for_each_in_subtree(callback);
}

IterationDecision CanonicalNavigable::for_each_in_subtree(Function<IterationDecision(CanonicalNavigable const&)> const& callback) const
{
    for (auto const& child : m_children) {
        if (child->for_each_in_inclusive_subtree(callback) == IterationDecision::Break)
            return IterationDecision::Break;
    }

    return IterationDecision::Continue;
}

WebContentPage& CanonicalNavigable::remote_host() const
{
    VERIFY(has_remote_host());
    return *active_document().host();
}

void CanonicalNavigable::hand_pending_webdriver_commands_to(WebContentPage& new_host)
{
    auto& traversable = top_level_traversable();
    auto old_host = traversable.page_hosting(*this);
    if (!old_host)
        return;
    if (auto view = traversable.view(); view.has_value())
        view->move_pending_webdriver_commands_to_new_host({}, id(), *old_host, new_host);
}

RefPtr<WebContentClient> CanonicalNavigable::process_to_host_for_site_isolation(CanonicalDocument const& document, Optional<URL::Origin> const& initiator_origin) const
{
    RefPtr<WebContentPage> page_holding_navigable = parent() ? reporting_page() : top_level_traversable().page_hosting(*this);
    RefPtr<WebContentClient> process_holding_navigable = page_holding_navigable ? &page_holding_navigable->client() : nullptr;

    // An agent runs in one process: its documents go where it is hosted.
    if (auto process = document.relevant_global_object().agent().hosting_process())
        return process;

    // Documents that are not isolated go with the page holding the navigable.
    auto mode = site_isolation_mode();
    if (mode == SiteIsolationMode::Disabled || (mode == SiteIsolationMode::TopLevel && parent()))
        return process_holding_navigable;

    // An opaque origin keys an agent cluster of its own, which nothing but the documents it was created from can
    // address: it goes with the initiator's agent. A traversable's document of an opaque origin that no such agent
    // hosts, as a file's opened from the browser's UI is, keeps the process of a document of an opaque origin.
    if (document.origin().is_opaque()) {
        if (initiator_origin.has_value()) {
            if (auto initiator_agent = document.browsing_context().top_level_browsing_context().group()->similar_origin_window_agent_for(*initiator_origin)) {
                if (auto process = initiator_agent->hosting_process())
                    return process;
            }
        }
        if (!parent() && active_document().origin().is_opaque())
            return process_holding_navigable;
    }

    // A traversable's first document takes the process its initial about:blank came with, unless that document
    // inherited its creator's origin and shares the creator's process, or a browsing context group switch leaves the
    // initial about:blank's group.
    if (!parent() && active_document().is_initial_about_blank() && active_document().origin().is_opaque() && &document.browsing_context() == &active_browsing_context())
        return process_holding_navigable;

    return nullptr;
}

// A process that has hosted local file content may read local files, so site isolation has it host no content a site
// could have made: only local file content, documents of its origin, documents local file content made from what it
// holds, and about:blank documents no other process can reach. Local file content goes to no other process.
// Likewise, a process that has hosted a WebUI's documents may speak for that WebUI, so it hosts only that WebUI's
// documents and about:blank documents no other process can reach, and a WebUI's documents go to no other process.
RefPtr<WebContentClient> CanonicalNavigable::process_to_host(CanonicalDocument const& document, Optional<URL::Origin> const& initiator_origin) const
{
    auto process = process_to_host_for_site_isolation(document, initiator_origin);

    // NB: Without site isolation, every document stays with the process holding its navigable, local file content and
    //     WebUI documents too, and that process may then read local files or speak for the WebUI for as long as it lives.
    if (site_isolation_mode() == SiteIsolationMode::Disabled)
        return process;
    auto process_has_hosted_local_file_content = process && process->has_hosted_local_file_content();
    auto process_has_hosted_web_ui = process && process->hosted_web_ui().has_value();

    if (document.web_ui_host().has_value())
        return process && process->hosted_web_ui() == document.web_ui_host() ? process : nullptr;
    if (document.is_local_file_content())
        return process_has_hosted_local_file_content ? process : nullptr;
    if (!process_has_hosted_local_file_content && !process_has_hosted_web_ui)
        return process;
    if (process_has_hosted_local_file_content && document.origin().is_file_origin())
        return process;

    // An agent runs in one process, and one the process hosts was let in with the documents it has there.
    if (document.relevant_global_object().agent().hosting_process() == process)
        return process;

    // An about:blank document of an agent no process hosts yet is empty, and nothing else can script it. The browser's
    // UI creates one between documents.
    auto const& url = document.creation_url();
    if (Web::HTML::url_matches_about_blank(url))
        return process;

    auto is_made_from_what_its_initiator_holds = Web::HTML::url_matches_about_srcdoc(url) || url.scheme() == "data"sv;
    if (process_has_hosted_local_file_content && is_made_from_what_its_initiator_holds && initiator_origin.has_value() && initiator_origin->is_file_origin())
        return process;
    return nullptr;
}

ErrorOr<NonnullRefPtr<WebContentPage>> CanonicalNavigable::obtain_page_to_host(CanonicalDocument const& document, Optional<URL::Origin> const& initiator_origin)
{
    auto page = TRY(obtain_page_to_host_in(process_to_host(document, initiator_origin)));
    if (document.is_local_file_content())
        page->client().set_has_hosted_local_file_content();
    if (auto const& web_ui_host = document.web_ui_host(); web_ui_host.has_value())
        page->client().set_hosted_web_ui(*web_ui_host);
    return page;
}

ErrorOr<NonnullRefPtr<WebContentPage>> CanonicalNavigable::obtain_page_to_host_in(RefPtr<WebContentClient> host)
{
    auto& traversable = top_level_traversable();
    if (!parent())
        return traversable.obtain_page_to_host_traversable(move(host));

    // A page beginning to host the navigable starts from a document standing in for the current entry's.
    auto current_entry_descriptor = [&] {
        // NB: Until the update for its creation, the navigable's entry is the one initializing it created.
        if (auto const* current_entry = traversable.session_history().get_the_target_history_entry(*this, traversable.session_history().current_step()))
            return current_entry->descriptor();
        return active_session_history_entry()->descriptor();
    };

    // The host takes the navigable's node over once the document it is to display is activated; until then, the page
    // hosting the displayed document keeps it.
    if (host && host == &reporting_page()->client()) {
        // The page holding the container populates the document in a provisional navigable while another page hosts
        // the displayed document.
        if (has_remote_host())
            host->async_begin_hosting_navigable(reporting_page()->id(), id(), current_entry_descriptor(), traversable.system_visibility_state());
        return *reporting_page();
    }
    if (host && has_remote_host() && host == &remote_host().client())
        return remote_host();

    // A process holds one page per tab, with the tab's whole graph: the process displaying the tab hosts a document
    // in the view's page, another process in the page it has for the tab, or in a page created for it.
    Web::PageId page_id;
    if (host && host->page_id_for_traversable(traversable).has_value()) {
        page_id = *host->page_id_for_traversable(traversable);
        host->async_begin_hosting_navigable(page_id, id(), current_entry_descriptor(), traversable.system_visibility_state());
    } else if (host) {
        page_id = Application::the().allocate_page_id();
        host->async_create_embedded_page(page_id, traversable.remote_navigable_graph(), id(), current_entry_descriptor(), traversable.system_visibility_state());
        host->register_embedded_page(page_id, traversable);
        traversable.represent_group_in(*host);
    } else {
        auto process = TRY(Application::the().launch_child_frame_web_content_process(reporting_page()->client().is_private(), traversable.remote_navigable_graph(), id(), current_entry_descriptor(), traversable.system_visibility_state()));
        host = move(process.client);
        page_id = process.page_id;
        host->register_embedded_page(page_id, traversable);
        traversable.represent_group_in(*host);
    }

    auto& page = *host->page(page_id);
    if (auto view = traversable.view(); view.has_value())
        view->prepare_page_for_navigable(page);
    return page;
}

ErrorOr<CanonicalNavigable::PageToPopulateDocument> CanonicalNavigable::obtain_page_to_populate_document(CanonicalSessionHistoryEntry& entry, NavigationLoader& loader, WebContentPage& page_that_created_navigation_params)
{
    auto response_document = loader.response_document();
    if (!response_document.has_value())
        return PageToPopulateDocument { page_that_created_navigation_params, nullptr };

    NonnullRefPtr document_state = entry.document_state;
    response_document->document_id = Application::the().allocate_ui_process_cross_process_id();
    auto document = create_and_initialize_a_document(*response_document);
    loader.set_document(*document, *this);
    populate_document(document_state, document, loader.result().inline_content_origin);

    // A document created for inline content stands in for the resource the process that fetched it could not load; that
    // process hosts it.
    NonnullRefPtr<WebContentPage> host = page_that_created_navigation_params;
    if (!response_document->is_inline_content || is_top_level_traversable()) {
        auto host_or_error = obtain_page_to_host(*document, document_state->initiator_origin);
        if (host_or_error.is_error()) {
            abandon_populated_document(document_state, *document);
            return host_or_error.release_error();
        }
        host = host_or_error.release_value();

        // Obtaining the page can replace the tab's process, whose change callbacks can abandon the document.
        if (!document_state->populated_document.has_value() || document_state->populated_document->document != document)
            return Error::from_string_literal("The populated document was abandoned");
    }

    // The host takes the navigable over when the document is activated, after the displayed document is unloaded.
    place_populated_document(*document, *host);
    return PageToPopulateDocument { move(host), move(document) };
}

RefPtr<CanonicalDocument> CanonicalNavigable::document_with_id(Web::HTML::CrossProcessId id) const
{
    if (active_document().id() == id)
        return active_document();
    RefPtr<CanonicalDocument> document;
    for_each_populated_document_state([&](CanonicalDocumentState const& document_state) {
        if (document_state.populated_document->document->id() == id)
            document = document_state.populated_document->document;
    });
    return document;
}

// A document state that is not a navigation's is a history job's, which the navigable holds while the job runs.
void CanonicalNavigable::populate_document(NonnullRefPtr<CanonicalDocumentState> document_state, NonnullRefPtr<CanonicalDocument> document, Optional<URL::Origin> inline_content_origin)
{
    document->determine_whether_it_is_local_file_content(blob_url_store());
    if (document->is_local_file_content())
        document_state->held_local_file_content = true;
    abandon_populated_document(document_state);
    auto is_for_navigation = any_of(m_navigations, [&](auto const& navigation) {
        return navigation->history_entry && navigation->history_entry->document_state == document_state;
    });
    if (!is_for_navigation) {
        if (m_document_state_populated_by_history_job)
            abandon_populated_document(m_document_state_populated_by_history_job.release_nonnull());
        m_document_state_populated_by_history_job = document_state;
    }
    document_state->populated_document = PopulatedDocument { move(document), move(inline_content_origin) };
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#navigate-to-a-javascript:-url
// NB: newDocument's URL is the active document's, as the process hosting both reports it. Whatever URL it reports,
//     newDocument holds no local file content and displays no WebUI unless the active document does, as its contents
//     come from that process.
void CanonicalNavigable::populate_document_for_javascript_url(CanonicalNavigation& navigation, NonnullRefPtr<CanonicalSessionHistoryEntry> history_entry, NonnullRefPtr<CanonicalDocument> document)
{
    document->determine_whether_it_is_local_file_content(blob_url_store());
    if (document->is_local_file_content() && !active_document().is_local_file_content())
        return;
    if (document->web_ui_host().has_value() && document->web_ui_host() != active_document().web_ui_host())
        return;
    navigation.history_entry = move(history_entry);
    populate_document(navigation.history_entry->document_state, move(document));
}

// The process hosting a document populated for the navigable created it with an origin other than its navigation
// params': a document for inline content in place of a response it found blocked, or the PDF viewer.
void CanonicalNavigable::did_create_populated_document_with_an_origin_of_its_own(WebContentPage const& host, Web::HTML::CrossProcessId document_id, Web::HTML::PopulatedDocumentOrigin populated_document_origin, Web::HTML::EnvironmentId const& environment_id)
{
    Optional<PopulatedDocument&> populated_document;
    for_each_populated_document_state([&](CanonicalDocumentState& document_state) {
        if (document_state.populated_document->document->id() == document_id)
            populated_document = *document_state.populated_document;
    });
    if (!populated_document.has_value() || populated_document->document->host() != &host)
        return;

    auto is_inline_content = populated_document_origin == Web::HTML::PopulatedDocumentOrigin::InlineContent;
    if (is_inline_content && !populated_document->inline_content_origin.has_value())
        return;
    // AD-HOC: The PDF viewer runs with an origin of its own.
    auto origin = is_inline_content ? *populated_document->inline_content_origin : URL::Origin { "resource"_string, String {}, {} };

    auto url = is_inline_content ? URL::about_error() : populated_document->document->creation_url();
    NavigationLoader::ResponseDocument response_document {
        .is_inline_content = is_inline_content,
        .coop_enforcement_result = { .url = url, .origin = origin, .opener_policy = {} },
        .response_url = url,
        .request_current_url = {},
        .origin = origin,
        .opener_policy = {},
        .embedder_policy = {},
        .final_sandboxing_flag_set = populated_document->document->active_sandboxing_flag_set(),
        .environment_id = environment_id,
        .document_id = document_id,
    };
    auto document = create_and_initialize_a_document(response_document);
    document->set_host(populated_document->document->host());
    populated_document->document = move(document);
}

void CanonicalNavigable::abandon_populated_document(NonnullRefPtr<CanonicalDocumentState> document_state, CanonicalDocument const& document)
{
    if (document_state->populated_document.has_value() && document_state->populated_document->document == &document)
        abandon_populated_document(move(document_state));
}

// A page created to host the abandoned document is discarded, unless it hosts another document of the navigable.
void CanonicalNavigable::abandon_populated_document(NonnullRefPtr<CanonicalDocumentState> document_state)
{
    if (m_document_state_populated_by_history_job == document_state)
        m_document_state_populated_by_history_job = nullptr;
    if (!document_state->populated_document.has_value())
        return;
    NonnullRefPtr document = document_state->populated_document->document;
    RefPtr<WebContentPage> host = document->host();
    document_state->populated_document.clear();
    if (!host || top_level_traversable().hosts(*this, *host) || pending_host_matches(*host))
        return;
    host->async_discard_provisional_navigable(id());
    top_level_traversable().end_history_jobs(*this, document.ptr());
    top_level_traversable().release_page_if_unused(host.release_nonnull());
}

void CanonicalNavigable::place_populated_document(CanonicalDocument& document, WebContentPage& page)
{
    document.set_host(page);
    send_viewport_to_host();
}

bool CanonicalNavigable::pending_host_matches(WebContentPage const& page) const
{
    bool matches = false;
    for_each_pending_host([&](WebContentPage const& host) {
        if (&host == &page)
            matches = true;
    });
    return matches;
}

void CanonicalNavigable::discard_pending_host()
{
    Vector<NonnullRefPtr<CanonicalDocumentState>> document_states;
    for_each_populated_document_state([&](CanonicalDocumentState& document_state) {
        document_states.append(document_state);
    });
    for (auto& document_state : document_states)
        abandon_populated_document(move(document_state));
}

void CanonicalNavigable::discard_pending_host(WebContentPage const& page)
{
    if (active_document().host() == &page)
        return;
    Vector<NonnullRefPtr<CanonicalDocumentState>> document_states;
    for_each_populated_document_state([&](CanonicalDocumentState& document_state) {
        if (document_state.populated_document->document->host() == &page)
            document_states.append(document_state);
    });
    for (auto& document_state : document_states)
        abandon_populated_document(move(document_state));
}

void CanonicalNavigable::set_stats_displayed(bool displayed)
{
    if (exchange(m_stats_displayed, displayed) != displayed)
        send_stats_displayed_to_hosts();
}

void CanonicalNavigable::send_stats_displayed_to_hosts() const
{
    // An embedding change also changes the eligibility of hosted descendants.
    RefPtr<WebContentPage> host = has_remote_host() ? &remote_host() : nullptr;
    if (host)
        send_stats_displayed_to(*host);
    for_each_pending_host([&](WebContentPage& pending_host) {
        if (&pending_host != host.ptr())
            send_stats_displayed_to(pending_host);
    });
    for (auto const& child : children())
        child->send_stats_displayed_to_hosts();
}

void CanonicalNavigable::send_stats_displayed_to(WebContentPage& host) const
{
    bool displayed = true;
    for (auto const* ancestor = this; ancestor->parent(); ancestor = ancestor->parent())
        displayed &= ancestor->m_stats_displayed;
    host.async_set_rendering_stats_displayed(id(), displayed);
}

void CanonicalNavigable::set_viewport(Web::DevicePixelRect viewport_rect, Web::DevicePixelRect viewport_intersection, double device_pixel_ratio)
{
    m_viewport_rect = viewport_rect;
    m_viewport_intersection = viewport_intersection;
    m_device_pixel_ratio = device_pixel_ratio;
    send_viewport_to_host();
}

void CanonicalNavigable::send_viewport_to_host() const
{
    if (!m_viewport_rect.has_value())
        return;
    RefPtr<WebContentPage> remote_host = has_remote_host() ? &this->remote_host() : nullptr;
    if (remote_host)
        send_viewport_to(*remote_host);
    for_each_pending_host([&](WebContentPage& host) {
        if (&host != remote_host.ptr())
            send_viewport_to(host);
    });
}

void CanonicalNavigable::send_viewport_to(WebContentPage& host) const
{
    host.async_set_hosted_root_viewport(id(), m_viewport_rect->size(), m_viewport_intersection, m_device_pixel_ratio);
    if (Core::StatsLog::the())
        send_stats_displayed_to(host);
}

void CanonicalNavigable::set_hosted_state(Web::HTML::HostedNavigableState state)
{
    m_document_blob_url = BlobURLHandle::for_url(blob_url_store(), active_document_url());
    m_hosted_state = move(state);
}

// The lost document neither paints into its container nor delays its load event.
void CanonicalNavigable::did_lose_active_document()
{
    active_document().set_host(nullptr);
    if (!m_hosted_state.has_value())
        return;
    m_hosted_state->compositor_context_id = {};
    m_hosted_state->delays_the_load_event_of_its_container = false;
}

void CanonicalNavigable::update_container_state(Web::HTML::ReplicatedContainerState state)
{
    if (!m_hosted_state.has_value())
        return;
    if (m_container_document) {
        state.document_active_sandboxing_flag_set = m_container_document->active_sandboxing_flag_set();
        state.document_embedder_policy = m_container_document->embedder_policy();
        state.document_permissions_policy = m_container_document->permissions_policy();
    }
    m_hosted_state->container = state;
    if (has_remote_host())
        remote_host().async_update_local_root_container_state(id(), move(state));
}

void CanonicalNavigable::update_hosted_state(Web::HTML::HostedNavigableState state)
{
    // A child's container is reported by the page holding it, not by the page hosting the child's document.
    if (m_container_document && m_hosted_state.has_value())
        state.container = m_hosted_state->container;
    set_hosted_state(move(state));
    send_replicated_state();
    if (is_top_level_traversable()) {
        if (auto view = top_level_traversable().view(); view.has_value())
            view->notify_content_blocking_changed();
    }
}

Optional<Web::HTML::ReplicatedNavigableState> CanonicalNavigable::replicated_state() const
{
    if (!m_hosted_state.has_value())
        return {};
    auto const& hosted_state = *m_hosted_state;
    auto const& traversable = top_level_traversable();
    auto& browsing_context = active_browsing_context();

    // The opener browsing context is replicated as the navigable it is active in.
    auto opener_browsing_context = browsing_context.opener_browsing_context();
    Optional<Web::HTML::CrossProcessId> opener_navigable_id;
    if (opener_browsing_context) {
        if (auto const* opener = CanonicalTraversable::navigable_with_active_browsing_context(*opener_browsing_context))
            opener_navigable_id = opener->id();
    }

    Optional<u64> browsing_context_group_id;
    if (auto group = traversable.active_browsing_context().group())
        browsing_context_group_id = group->id();

    return Web::HTML::ReplicatedNavigableState {
        .target_name = target_name(),
        .active_document_url = active_document_url(),
        .active_document_origin = active_document().origin(),
        .active_document_is_fully_active = hosted_state.active_document_is_fully_active,
        .top_level_creation_url = active_document().relevant_global_object().relevant_settings_object().top_level_creation_url().value(),
        .top_level_origin = traversable.active_document().origin(),
        .top_level_is_created_by_web_content = traversable.is_created_by_web_content(),
        .top_level_session_history_entry_count = traversable.session_history().size(),
        .has_cross_site_ancestor = active_document_has_cross_site_ancestor(),
        .browsing_context_group_id = browsing_context_group_id,
        .opener_policy = active_document().opener_policy(),
        .active_browsing_context_is_auxiliary = browsing_context.is_auxiliary(),
        .opener_navigable_id = opener_navigable_id,
        .active_document_is_completely_loaded = active_document().is_completely_loaded(),
        .is_closing = hosted_state.is_closing,
        .container = hosted_state.container,
        .delays_the_load_event_of_its_container = hosted_state.delays_the_load_event_of_its_container,
        .has_session_history_entry_and_ready_for_navigation = has_session_history_entry_and_ready_for_navigation(),
        .compositor_context_id = hosted_state.compositor_context_id,
    };
}

void CanonicalNavigable::send_replicated_state() const
{
    auto state = replicated_state();
    if (!state.has_value())
        return;
    top_level_traversable().for_each_page_representing(*this, [&](WebContentPage& page) {
        page.async_update_remote_navigable(id(), *state);
    });
}

// The process hosting the active document sets and disowns its browsing context's opener browsing context, which it
// reports as the navigable that browsing context is active in.
void CanonicalNavigable::did_set_opener_browsing_context(Optional<Web::HTML::CrossProcessId> opener_navigable_id)
{
    RefPtr<CanonicalBrowsingContext> opener_browsing_context;
    if (opener_navigable_id.has_value()) {
        if (auto* opener_traversable = CanonicalTraversable::traversable_containing(*opener_navigable_id)) {
            if (auto opener = opener_traversable->find(*opener_navigable_id); opener.has_value())
                opener_browsing_context = opener->active_browsing_context();
        }
    }
    active_browsing_context().set_opener_browsing_context(opener_browsing_context);

    auto& traversable = top_level_traversable();
    Vector<NonnullRefPtr<WebContentClient>> clients;
    traversable.for_each_hosting_page([&](WebContentPage& page) {
        if (!any_of(clients, [&](auto const& client) { return client.ptr() == &page.client(); }))
            clients.append(page.client());
    });

    // Every process holding part of the tab holds the tab of a new opener before it hears of it.
    if (opener_browsing_context) {
        for (auto& client : clients)
            traversable.represent_group_in(client);
    }

    send_replicated_state();

    // A process can stop needing the tab of the previous opener.
    for (auto& client : clients)
        client->release_unneeded_representing_pages();
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#script-settings-for-window-objects:concept-settings-object-has-cross-site-ancestor
bool CanonicalNavigable::active_document_has_cross_site_ancestor() const
{
    // 1. If window's navigable's parent is null, then return false.
    auto const* parent = this->parent();
    if (!parent)
        return false;

    // 2. Let parentDocument be window's navigable's parent's active document.
    // 3. If parentDocument's relevant settings object's has cross-site ancestor is true, then return true.
    if (parent->active_document_has_cross_site_ancestor())
        return true;

    // 4. If parentDocument's origin is not same site with window's associated Document's origin, then return true.
    if (!parent->active_document().origin().is_same_site(active_document().origin()))
        return true;

    // 5. Return false.
    return false;
}

bool CanonicalNavigable::has_session_history_entry_and_ready_for_navigation() const
{
    // The traversable's initial entry is among its session history entries from its creation; a child's is once the
    // steps its creation appended have added its nested history.
    return is_top_level_traversable() || top_level_traversable().session_history().get_session_history_entries(*this).has_value();
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#script-closable
bool CanonicalNavigable::is_script_closable() const
{
    // A navigable is script-closable if it is a top-level traversable, and any of the following are true:
    // - its is created by web content is true; or
    // - its session history entries's size is 1.
    if (!is_top_level_traversable())
        return false;

    auto const& traversable = top_level_traversable();
    return traversable.is_created_by_web_content()
        || traversable.session_history().size() == 1;
}

void CanonicalNavigable::document_completely_finished_loading(CanonicalDocument& document)
{
    document.set_completely_loaded();
    if (&document != &active_document())
        return;
    send_replicated_state();
    send_completely_finished_loading_to_container();
}

void CanonicalNavigable::send_completely_finished_loading_to_container() const
{
    top_level_traversable().for_each_page_representing(*this, [&](WebContentPage& page) {
        page.async_content_navigable_completely_finished_loading(id());
    });
}

bool CanonicalNavigable::current_session_history_entry_is(CanonicalSessionHistoryEntry const& entry) const
{
    return m_current_session_history_entry == &entry;
}

bool CanonicalNavigable::active_document_is(CanonicalSessionHistoryEntry const& entry) const
{
    return m_active_session_history_entry && entry.document_state->document == &active_document();
}

void CanonicalNavigable::did_commit_navigation(CanonicalSessionHistoryEntry& entry, Web::HTML::HostedNavigableState hosted_state, u64 operation_sequence_number, Optional<Utf16String const&> navigation_id, RefPtr<WebContentPage> host)
{
    // A navigation admitted after the operation, such as one admitted during a same-document traversal, is newer than it.
    auto ongoing_navigation_id = this->ongoing_navigation_id();
    auto ongoing_navigation = ongoing_navigation_id.has_value() ? navigation_with_id(*ongoing_navigation_id) : nullptr;
    auto commits_ongoing_navigation = !ongoing_navigation
        || (navigation_id.has_value()
                ? *navigation_id == ongoing_navigation->navigation_id
                : ongoing_navigation->sequence_number < operation_sequence_number);

    auto active_document_changed = !active_document_is(entry);
    NonnullRefPtr previous_document = active_document();

    // The document populated for the entry becomes its document state's document below.
    // NB: The process hosting the document ran the steps populating it for the entry it holds.
    entry.document_state->save_the_origin_of_the_populated_document();
    RefPtr<CanonicalDocument> document;
    if (auto populated_document = exchange(entry.document_state->populated_document, {}); populated_document.has_value())
        document = populated_document->document;
    if (m_document_state_populated_by_history_job == entry.document_state)
        m_document_state_populated_by_history_job = nullptr;
    VERIFY(document || !active_document_changed);
    if (!document)
        document = previous_document;

    // The commands WebDriver has waiting in the page that hosted the displaced document follow the navigable to the
    // page hosting the activated one.
    if (document != previous_document && host && host != previous_document->host())
        hand_pending_webdriver_commands_to(*host);

    RefPtr<WebContentPage> previous_display_page;
    if (is_top_level_traversable())
        previous_display_page = top_level_traversable().display_page();

    // A document state holds its document while its entry is active.
    if (m_active_session_history_entry->document_state != entry.document_state)
        m_active_session_history_entry->document_state->document = nullptr;
    entry.document_state->document = document;
    m_active_session_history_entry = entry;

    if (document != previous_document) {
        document->make_active();
        if (!document->host())
            document->set_host(host);
        // NB: A browsing context group switch discarded the previous document's browsing context. It is removed from
        //     its group once the switch is committed, as the navigation can be canceled until then.
        if (is_top_level_traversable() && &document->browsing_context() != &previous_document->browsing_context()) {
            previous_document->browsing_context().remove();
            top_level_traversable().did_switch_browsing_context_group();
        }
    }
    // The tab is displayed by the page hosting its document.
    if (is_top_level_traversable()) {
        auto& traversable = top_level_traversable();
        if (document->host())
            traversable.did_activate_document_in({}, *document->host());
        if (auto view = traversable.view(); view.has_value() && traversable.display_page() != previous_display_page)
            view->did_change_display_page({}, previous_display_page);
    }
    update_hosted_state(move(hosted_state));
    if (document != previous_document && document->is_completely_loaded())
        send_completely_finished_loading_to_container();

    // The displaced document is gone, and its child navigables with it. When another page hosts the activated
    // document, the page that hosted the displaced one holds the navigable remotely from now on.
    if (document != previous_document) {
        auto& traversable = top_level_traversable();
        auto previous_host = previous_document->host();
        traversable.remove_child_navigables_of(*this, *previous_document);
        if (previous_host && previous_host != document->host() && previous_host->is_open())
            traversable.stop_hosting_in_page(*this, *previous_host);
        // The Picture-in-Picture window of the displaced document, or of a document it embedded, closes with it.
        PictureInPictureManager::the().active_documents_did_change();
        // Whether a process may capture audio depends on the documents it hosts.
        for (auto const& page : { previous_host, document->host() }) {
            if (page && page->is_open())
                page->client().update_audio_capture_permission();
        }
    }

    // A navigation can commit while a newer navigation is already in flight. In that case update the replicated
    // state for the committed document, and leave the newer navigation alone.
    if (!commits_ongoing_navigation)
        return;

    // The activated document's load becomes the navigable's tracked load. A same-document activation leaves the active
    // document's load in place.
    if (document != previous_document) {
        Optional<Utf16String> load_navigation_id;
        if (navigation_id.has_value())
            load_navigation_id = *navigation_id;
        else if (ongoing_navigation)
            load_navigation_id = ongoing_navigation->navigation_id;
        m_active_document_load = ActiveDocumentLoad { .navigation_id = move(load_navigation_id) };
    }

    clear_ongoing_navigation();
}

Optional<Utf16String const&> CanonicalNavigable::ongoing_navigation_id() const
{
    if (auto const* navigation_id = m_ongoing_navigation.get_pointer<Utf16String>())
        return *navigation_id;
    return {};
}

RefPtr<CanonicalNavigation> CanonicalNavigable::navigation_with_id(Utf16String const& navigation_id) const
{
    for (auto const& navigation : m_navigations) {
        if (navigation->navigation_id == navigation_id)
            return navigation;
    }
    return nullptr;
}

RefPtr<CanonicalNavigation> CanonicalNavigable::uncommitted_navigation() const
{
    auto navigation_id = ongoing_navigation_id();
    if (!navigation_id.has_value())
        return nullptr;
    auto navigation = navigation_with_id(*navigation_id);
    return navigation && !navigation->is_finalizing() ? navigation : nullptr;
}

RefPtr<CanonicalNavigation> CanonicalNavigable::navigation_being_finalized() const
{
    for (size_t i = m_navigations.size(); i > 0; --i) {
        if (m_navigations[i - 1]->is_finalizing())
            return m_navigations[i - 1];
    }
    return nullptr;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#set-the-ongoing-navigation
// NB: The steps of a navigation the ongoing navigation no longer names end.
void CanonicalNavigable::set_the_ongoing_navigation(OngoingNavigation value)
{
    // NB: Whatever the ongoing navigation becomes, the navigation start the pending blob URL handle covers has been
    //     announced, and its navigation holds a handle of its own, or is not coming.
    m_pending_navigation_blob_url = {};

    // 1. If navigable's ongoing navigation is equal to newValue, then return.
    if (m_ongoing_navigation == value)
        return;

    // 2. Inform the navigation API about aborting navigation given navigable.
    // NB: The page holding navigable does this as it sets its own ongoing navigation.

    auto superseded_navigation = uncommitted_navigation();

    // 3. Set navigable's ongoing navigation to newValue.
    m_ongoing_navigation = move(value);
    notify_view_of_ongoing_navigation();

    if (superseded_navigation && superseded_navigation->ends_when_superseded())
        end_navigation(*superseded_navigation);
}

void CanonicalNavigable::set_ongoing_navigation(CanonicalNavigation::Start navigation)
{
    if (auto* log = Core::StatsLog::the(); log && is_top_level_traversable()) {
        JsonObject data;
        data.set("navigable_id"sv, MUST(String::formatted("{}", id())));
        data.set("navigation_id"sv, navigation.navigation_id.to_utf8());
        data.set("url"sv, navigation.url.serialize());
        log->event("navigation_start"sv, move(data));
    }

    // NB: Taken before the handle covering this navigation's start is dropped below, so that a revoked entry is not
    //     let go of in between.
    auto blob_url = BlobURLHandle::for_url(blob_url_store(), navigation.url);

    auto navigation_id = navigation.navigation_id;
    auto ongoing_navigation = CanonicalNavigation::create(move(navigation));
    ongoing_navigation->blob_url = move(blob_url);
    m_navigations.append(move(ongoing_navigation));
    set_the_ongoing_navigation(move(navigation_id));
}

// Navigate, step 20, in the page hosting navigable's active document.
void CanonicalNavigable::begin_evaluating_javascript_url(URL::URL const& url, Utf16String const& navigation_id, WebContentPage& host)
{
    set_ongoing_navigation({
        .url = url,
        .navigation_id = navigation_id,
        .sequence_number = top_level_traversable().next_sequence_number(),
        .state = CanonicalNavigation::EvaluatingJavaScriptURL {},
        .page = host,
    });
    if (is_top_level_traversable() && host.displays_tab())
        host.begin_top_level_load(url);
}

void CanonicalNavigable::set_ongoing_navigation_to_traversal(Web::HTML::CrossProcessId operation_id)
{
    set_the_ongoing_navigation(Traversal { operation_id });
}

void CanonicalNavigable::clear_ongoing_navigation_traversal(Web::HTML::CrossProcessId operation_id)
{
    if (auto const* traversal = m_ongoing_navigation.get_pointer<Traversal>(); traversal && traversal->operation_id == operation_id) {
        m_ongoing_navigation = Empty {};
        notify_view_of_ongoing_navigation();
    }
}

void CanonicalNavigable::notify_view_of_ongoing_navigation() const
{
    if (!is_top_level_traversable())
        return;
    if (auto view = top_level_traversable().view(); view.has_value())
        view->did_change_ongoing_navigation({});
}

void CanonicalNavigable::clear_ongoing_navigation()
{
    set_the_ongoing_navigation(Empty {});
}

void CanonicalNavigable::end_navigation(CanonicalNavigation& navigation)
{
    if (auto navigation_id = ongoing_navigation_id(); navigation_id.has_value() && *navigation_id == navigation.navigation_id) {
        m_ongoing_navigation = Empty {};
        notify_view_of_ongoing_navigation();
    }
    auto index = m_navigations.find_first_index_if([&](auto const& candidate) { return candidate.ptr() == &navigation; });
    VERIFY(index.has_value());
    auto ended_navigation = m_navigations.take(*index);

    // The page checking if unloading is canceled holds the navigation until it is told to continue it.
    if (auto worker = navigation.worker(); worker && worker->is_open() && navigation.state.has<CanonicalNavigation::CheckingIfUnloadingIsCanceled>())
        worker->async_cancel_navigation_params_creation(id(), navigation.navigation_id);

    // The document populated for an ended navigation is not going to be activated.
    if (navigation.history_entry)
        abandon_populated_document(navigation.history_entry->document_state);
}

BlobURLStore* CanonicalNavigable::blob_url_store() const
{
    auto page = reporting_page();
    return page ? page->client().session().blob_url_store.ptr() : nullptr;
}

void CanonicalNavigable::retain_blob_url_token(URL::BlobURLEntry::Token token)
{
    if (auto* store = blob_url_store())
        m_pending_navigation_blob_url = BlobURLHandle { *store, token };
}

void CanonicalNavigable::cancel_navigation_for_client(WebContentClient& client)
{
    auto is_page_of_client = [&](RefPtr<WebContentPage> const& page) {
        return page && &page->client() == &client;
    };
    Vector<NonnullRefPtr<CanonicalNavigation>> canceled_navigations;
    for (auto const& navigation : m_navigations) {
        if (!navigation->is_finalizing() && (is_page_of_client(navigation->worker()) || is_page_of_client(navigation->host())))
            canceled_navigations.append(navigation);
    }
    for (auto const& navigation : canceled_navigations)
        end_navigation(*navigation);
}

// The history operation finalizing a navigation is over.
void CanonicalNavigable::did_finish_finalizing_navigation(Utf16String const& navigation_id)
{
    auto navigation = navigation_with_id(navigation_id);
    if (!navigation || !navigation->is_finalizing())
        return;
    end_navigation(*navigation);
}

void CanonicalNavigable::did_cancel_navigation(Utf16String navigation_id)
{
    auto navigation = navigation_with_id(navigation_id);
    auto canceled_uncommitted_navigation = navigation && !navigation->is_finalizing();
    auto is_tracked_load = tracked_load_navigation_id() == navigation_id;
    if (canceled_uncommitted_navigation)
        end_navigation(*navigation);
    else if (is_tracked_load)
        clear_active_document_load();

    if (!is_tracked_load || !is_top_level_traversable())
        return;
    // A navigation being finalized that is canceled never activates its document, like an uncommitted one.
    if (auto view = top_level_traversable().view(); view.has_value())
        view->did_cancel_loading({}, navigation != nullptr);
}

Optional<Utf16String> CanonicalNavigable::tracked_load_navigation_id() const
{
    if (auto navigation_id = ongoing_navigation_id(); navigation_id.has_value())
        return *navigation_id;
    if (auto navigation = navigation_being_finalized())
        return navigation->navigation_id;
    return m_active_document_load.navigation_id;
}

}
