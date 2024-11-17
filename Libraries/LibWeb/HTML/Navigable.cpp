/*
 * Copyright (c) 2022-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/SystemColor.h>
#include <LibWeb/Crypto/Crypto.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentLoading.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/URL.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/DocumentState.h>
#include <LibWeb/HTML/HTMLIFrameElement.h>
#include <LibWeb/HTML/HistoryHandlingBehavior.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/Navigation.h>
#include <LibWeb/HTML/NavigationObserver.h>
#include <LibWeb/HTML/NavigationParams.h>
#include <LibWeb/HTML/POSTResource.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/SandboxingFlagSet.h>
#include <LibWeb/HTML/Scripting/ClassicScript.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Loader/GeneratedPagesLoader.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/Painting/ViewportPaintable.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Selection/Selection.h>
#include <LibWeb/XHR/FormData.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(Navigable);

class ResponseHolder : public JS::Cell {
    GC_CELL(ResponseHolder, JS::Cell);
    GC_DECLARE_ALLOCATOR(ResponseHolder);

public:
    [[nodiscard]] static GC::Ref<ResponseHolder> create(JS::VM& vm)
    {
        return vm.heap().allocate<ResponseHolder>();
    }

    [[nodiscard]] GC::Ptr<Fetch::Infrastructure::Response> response() const { return m_response; }
    void set_response(GC::Ptr<Fetch::Infrastructure::Response> response) { m_response = response; }

    virtual void visit_edges(Cell::Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_response);
    }

private:
    GC::Ptr<Fetch::Infrastructure::Response> m_response;
};

GC_DEFINE_ALLOCATOR(ResponseHolder);

HashTable<Navigable*>& all_navigables()
{
    static HashTable<Navigable*> set;
    return set;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#child-navigable
Vector<GC::Root<Navigable>> Navigable::child_navigables() const
{
    Vector<GC::Root<Navigable>> results;
    for (auto& entry : all_navigables()) {
        if (entry->current_session_history_entry()->step() == SessionHistoryEntry::Pending::Tag)
            continue;
        if (entry->parent() == this)
            results.append(entry);
    }

    return results;
}

bool Navigable::is_traversable() const
{
    return is<TraversableNavigable>(*this);
}

bool Navigable::is_ancestor_of(GC::Ref<Navigable> other) const
{
    for (auto ancestor = other->parent(); ancestor; ancestor = ancestor->parent()) {
        if (ancestor == this)
            return true;
    }
    return false;
}

Navigable::Navigable(GC::Ref<Page> page)
    : m_page(page)
    , m_event_handler({}, *this)
{
    all_navigables().set(this);
}

Navigable::~Navigable()
{
    all_navigables().remove(this);
}

void Navigable::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_page);
    visitor.visit(m_parent);
    visitor.visit(m_current_session_history_entry);
    visitor.visit(m_active_session_history_entry);
    visitor.visit(m_container);
    visitor.visit(m_navigation_observers);
    m_event_handler.visit_edges(visitor);
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#script-closable
bool Navigable::is_script_closable()
{
    // A navigable is script-closable if its active browsing context is an auxiliary browsing context that was created
    // by a script (as opposed to by an action of the user), or if it is a top-level traversable whose session history
    // entries's size is 1.
    if (auto browsing_context = active_browsing_context(); browsing_context && browsing_context->is_auxiliary())
        return true;

    if (is_top_level_traversable())
        return get_session_history_entries().size() == 1;

    return false;
}

void Navigable::set_delaying_load_events(bool value)
{
    if (value) {
        auto document = container_document();
        VERIFY(document);
        m_delaying_the_load_event.emplace(*document);
    } else {
        m_delaying_the_load_event.clear();
    }
}

GC::Ptr<Navigable> Navigable::navigable_with_active_document(GC::Ref<DOM::Document> document)
{
    for (auto* navigable : all_navigables()) {
        if (navigable->active_document() == document)
            return navigable;
    }
    return nullptr;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#initialize-the-navigable
ErrorOr<void> Navigable::initialize_navigable(GC::Ref<DocumentState> document_state, GC::Ptr<Navigable> parent)
{
    static int next_id = 0;
    m_id = String::number(next_id++);

    // 1. Assert: documentState's document is non-null.
    VERIFY(document_state->document());

    // 2. Let entry be a new session history entry, with
    GC::Ref<SessionHistoryEntry> entry = *heap().allocate<SessionHistoryEntry>();
    // URL: document's URL
    entry->set_url(document_state->document()->url());
    // document state: documentState
    entry->set_document_state(document_state);

    // 3. Set navigable's current session history entry to entry.
    m_current_session_history_entry = entry;

    // 4. Set navigable's active session history entry to entry.
    m_active_session_history_entry = entry;

    // 5. Set navigable's parent to parent.
    m_parent = parent;

    return {};
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-the-target-history-entry
GC::Ptr<SessionHistoryEntry> Navigable::get_the_target_history_entry(int target_step) const
{
    // 1. Let entries be the result of getting session history entries for navigable.
    auto& entries = get_session_history_entries();

    // 2. Return the item in entries that has the greatest step less than or equal to step.
    GC::Ptr<SessionHistoryEntry> result = nullptr;
    for (auto& entry : entries) {
        auto entry_step = entry->step().get<int>();
        if (entry_step <= target_step) {
            if (!result || result->step().get<int>() < entry_step) {
                result = entry;
            }
        }
    }

    return result;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#activate-history-entry
void Navigable::activate_history_entry(GC::Ptr<SessionHistoryEntry> entry)
{
    // FIXME: 1. Save persisted state to the navigable's active session history entry.

    // 2. Let newDocument be entry's document.
    GC::Ptr<DOM::Document> new_document = entry->document().ptr();

    // 3. Assert: newDocument's is initial about:blank is false, i.e., we never traverse
    //    back to the initial about:blank Document because it always gets replaced when we
    //    navigate away from it.
    VERIFY(!new_document->is_initial_about_blank());

    // 4. Set navigable's active session history entry to entry.
    m_active_session_history_entry = entry;

    // 5. Make active newDocument.
    new_document->make_active();

    if (m_ongoing_navigation.has<Empty>()) {
        for (auto navigation_observer : m_navigation_observers) {
            if (navigation_observer->navigation_complete())
                navigation_observer->navigation_complete()->function()();
        }
    }
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#nav-document
GC::Ptr<DOM::Document> Navigable::active_document()
{
    // A navigable's active document is its active session history entry's document.
    return m_active_session_history_entry->document();
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#nav-bc
GC::Ptr<BrowsingContext> Navigable::active_browsing_context()
{
    // A navigable's active browsing context is its active document's browsing context.
    // If this navigable is a traversable navigable, then its active browsing context will be a top-level browsing context.
    if (auto document = active_document())
        return document->browsing_context();
    return nullptr;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#nav-wp
GC::Ptr<HTML::WindowProxy> Navigable::active_window_proxy()
{
    // A navigable's active WindowProxy is its active browsing context's associated WindowProxy.
    if (auto browsing_context = active_browsing_context())
        return browsing_context->window_proxy();
    return nullptr;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#nav-window
GC::Ptr<HTML::Window> Navigable::active_window()
{
    // A navigable's active window is its active WindowProxy's [[Window]].
    if (auto window_proxy = active_window_proxy())
        return window_proxy->window();
    return nullptr;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#nav-target
String Navigable::target_name() const
{
    // A navigable's target name is its active session history entry's document state's navigable target name.
    return active_session_history_entry()->document_state()->navigable_target_name();
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#nav-container
GC::Ptr<NavigableContainer> Navigable::container() const
{
    // The container of a navigable navigable is the navigable container whose nested navigable is navigable, or null if there is no such element.
    return NavigableContainer::navigable_container_with_content_navigable(const_cast<Navigable&>(*this));
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#nav-container-document
GC::Ptr<DOM::Document> Navigable::container_document() const
{
    auto container = this->container();

    // 1. If navigable's container is null, then return null.
    if (!container)
        return nullptr;

    // 2. Return navigable's container's node document.
    return container->document();
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#nav-traversable
GC::Ptr<TraversableNavigable> Navigable::traversable_navigable() const
{
    // 1. Let navigable be inputNavigable.
    auto navigable = const_cast<Navigable*>(this);

    // 2. While navigable is not a traversable navigable, set navigable to navigable's parent.
    while (navigable && !is<TraversableNavigable>(*navigable))
        navigable = navigable->parent();

    // 3. Return navigable.
    return static_cast<TraversableNavigable*>(navigable);
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#nav-top
GC::Ptr<TraversableNavigable> Navigable::top_level_traversable()
{
    // 1. Let navigable be inputNavigable.
    auto navigable = this;

    // 2. While navigable's parent is not null, set navigable to navigable's parent.
    while (navigable->parent())
        navigable = navigable->parent();

    // 3. Return navigable.
    return verify_cast<TraversableNavigable>(navigable);
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#set-the-ongoing-navigation
void Navigable::set_ongoing_navigation(Variant<Empty, Traversal, String> ongoing_navigation)
{
    // 1. If navigable's ongoing navigation is equal to newValue, then return.
    if (m_ongoing_navigation == ongoing_navigation)
        return;

    // 2. Inform the navigation API about aborting navigation given navigable.
    inform_the_navigation_api_about_aborting_navigation();

    // 3. Set navigable's ongoing navigation to newValue.
    m_ongoing_navigation = ongoing_navigation;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#the-rules-for-choosing-a-navigable
Navigable::ChosenNavigable Navigable::choose_a_navigable(StringView name, TokenizedFeature::NoOpener no_opener, ActivateTab activate_tab, Optional<TokenizedFeature::Map const&> window_features)
{
    // NOTE: Implementation for step 7 here.
    GC::Ptr<Navigable> same_name_navigable = nullptr;
    if (!Infra::is_ascii_case_insensitive_match(name, "_blank"sv)) {
        for (auto& n : all_navigables()) {
            if (n->target_name() == name && !n->has_been_destroyed()) {
                same_name_navigable = n;
            }
        }
    }

    // 1. Let chosen be null.
    GC::Ptr<Navigable> chosen = nullptr;

    // 2. Let windowType be "existing or none".
    auto window_type = WindowType::ExistingOrNone;

    // 3. Let sandboxingFlagSet be current's active document's active sandboxing flag set.
    auto sandboxing_flag_set = active_document()->active_sandboxing_flag_set();

    // 4. If name is the empty string or an ASCII case-insensitive match for "_self", then set chosen to currentNavigable.
    if (name.is_empty() || Infra::is_ascii_case_insensitive_match(name, "_self"sv)) {
        chosen = this;
    }

    // 5. Otherwise, if name is an ASCII case-insensitive match for "_parent",
    //    set chosen to currentNavigable's parent, if any, and currentNavigable otherwise.
    else if (Infra::is_ascii_case_insensitive_match(name, "_parent"sv)) {
        if (auto parent = this->parent())
            chosen = parent;
        else
            chosen = this;
    }

    // 6. Otherwise, if name is an ASCII case-insensitive match for "_top",
    //    set chosen to currentNavigable's traversable navigable.
    else if (Infra::is_ascii_case_insensitive_match(name, "_top"sv)) {
        chosen = traversable_navigable();
    }

    //  7. Otherwise, if name is not an ASCII case-insensitive match for "_blank",
    //     there exists a navigable whose target name is the same as name, currentNavigable's
    //     active browsing context is familiar with that navigable's active browsing context,
    //     and the user agent determines that the two browsing contexts are related enough that
    //     it is ok if they reach each other, set chosen to that navigable. If there are multiple
    //     matching navigables, the user agent should pick one in some arbitrary consistent manner,
    //     such as the most recently opened, most recently focused, or more closely related, and set
    //     chosen to it.
    else if (same_name_navigable != nullptr && (active_browsing_context()->is_familiar_with(*same_name_navigable->active_browsing_context()))) {
        // FIXME: Handle multiple name-match case
        // FIXME: When are these contexts 'not related enough' ?
        chosen = same_name_navigable;
    }

    // 8. Otherwise, a new top-level traversable is being requested, and what happens depends on the
    // user agent's configuration and abilities — it is determined by the rules given for the first
    // applicable option from the following list:
    else {
        // --> If current's active window does not have transient activation and the user agent has been configured to
        //     not show popups (i.e., the user agent has a "popup blocker" enabled)
        if (active_window() && !active_window()->has_transient_activation() && traversable_navigable()->page().should_block_pop_ups()) {
            // FIXME: The user agent may inform the user that a popup has been blocked.
            dbgln("Pop-up blocked!");
        }

        // --> If sandboxingFlagSet has the sandboxed auxiliary navigation browsing context flag set
        else if (has_flag(sandboxing_flag_set, SandboxingFlagSet::SandboxedAuxiliaryNavigation)) {
            // FIXME: The user agent may report to a developer console that a popup has been blocked.
            dbgln("Pop-up blocked!");
        }

        // --> If the user agent has been configured such that in this instance it will create a new top-level traversable
        else if (true) { // FIXME: When is this the case?
            // 1. Set windowType to "new and unrestricted".
            window_type = WindowType::NewAndUnrestricted;

            // 2. Let currentDocument be currentNavigable's active document.
            auto current_document = active_document();

            // 3. If currentDocument's opener policy's value is "same-origin" or "same-origin-plus-COEP",
            //    and currentDocument's origin is not same origin with currentDocument's relevant settings object's top-level origin, then:
            if ((current_document->opener_policy().value == OpenerPolicyValue::SameOrigin || current_document->opener_policy().value == OpenerPolicyValue::SameOriginPlusCOEP)
                && !current_document->origin().is_same_origin(relevant_settings_object(*current_document).top_level_origin)) {

                // 1. Set noopener to true.
                no_opener = TokenizedFeature::NoOpener::Yes;

                // 2. Set name to "_blank".
                name = "_blank"sv;

                // 3. Set windowType to "new with no opener".
                window_type = WindowType::NewWithNoOpener;
            }
            // NOTE: In the presence of an opener policy,
            //       nested documents that are cross-origin with their top-level browsing context's active document always set noopener to true.

            // 4. Let chosen be null.
            chosen = nullptr;

            // 5. Let targetName be the empty string.
            String target_name;

            // 6. If name is not an ASCII case-insensitive match for "_blank", then set targetName to name.
            if (!Infra::is_ascii_case_insensitive_match(name, "_blank"sv))
                target_name = MUST(String::from_utf8(name));

            auto create_new_traversable_closure = [this, no_opener, target_name, activate_tab, window_features](GC::Ptr<BrowsingContext> opener) -> GC::Ref<Navigable> {
                auto hints = WebViewHints::from_tokenised_features(window_features.value_or({}), traversable_navigable()->page());
                auto [page, window_handle] = traversable_navigable()->page().client().page_did_request_new_web_view(activate_tab, hints, no_opener);
                auto traversable = TraversableNavigable::create_a_new_top_level_traversable(*page, opener, target_name).release_value_but_fixme_should_propagate_errors();
                page->set_top_level_traversable(traversable);
                traversable->set_window_handle(window_handle);
                return traversable;
            };
            auto create_new_traversable = GC::create_function(heap(), move(create_new_traversable_closure));

            // 7. If noopener is true, then set chosen to the result of creating a new top-level traversable given null and targetName.
            if (no_opener == TokenizedFeature::NoOpener::Yes) {
                chosen = create_new_traversable->function()(nullptr);
            }

            // 8. Otherwise:
            else {
                // 1. Set chosen to the result of creating a new top-level traversable given currentNavigable's active browsing context and targetName.
                chosen = create_new_traversable->function()(active_browsing_context());

                // FIXME: 2. If sandboxingFlagSet's sandboxed navigation browsing context flag is set,
                //    then set chosen's active browsing context's one permitted sandboxed navigator to currentNavigable's active browsing context.
            }

            // FIXME: 5. If sandboxingFlagSet's sandbox propagates to auxiliary browsing contexts flag is set,
            //    then all the flags that are set in sandboxingFlagSet must be set in chosen's active browsing context's popup sandboxing flag set.
            // Our BrowsingContexts do not have SandboxingFlagSets yet, only documents do
        }

        // --> If the user agent has been configured such that in this instance t will reuse current
        else if (false) { // FIXME: When is this the case?
            // Set chosen to current.
            chosen = *this;
        }

        // --> If the user agent has been configured such that in this instance it will not find a browsing context
        else if (false) { // FIXME: When is this the case?
            // Do nothing.
        }
    }

    return { chosen.ptr(), window_type };
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-session-history-entries
Vector<GC::Ref<SessionHistoryEntry>>& Navigable::get_session_history_entries() const
{
    // 1. Let traversable be navigable's traversable navigable.
    auto traversable = traversable_navigable();

    // FIXME 2. Assert: this is running within traversable's session history traversal queue.

    // 3. If navigable is traversable, return traversable's session history entries.
    if (this == traversable)
        return traversable->session_history_entries();

    // 4. Let docStates be an empty ordered set of document states.
    Vector<GC::Ptr<DocumentState>> doc_states;

    // 5. For each entry of traversable's session history entries, append entry's document state to docStates.
    for (auto& entry : traversable->session_history_entries())
        doc_states.append(entry->document_state());

    // 6. For each docState of docStates:
    while (!doc_states.is_empty()) {
        auto doc_state = doc_states.take_first();

        // 1. For each nestedHistory of docState's nested histories:
        for (auto& nested_history : doc_state->nested_histories()) {
            // 1. If nestedHistory's id equals navigable's id, return nestedHistory's entries.
            if (nested_history.id == id())
                return nested_history.entries;

            // 2. For each entry of nestedHistory's entries, append entry's document state to docStates.
            for (auto& entry : nested_history.entries)
                doc_states.append(entry->document_state());
        }
    }

    VERIFY_NOT_REACHED();
}

// https://html.spec.whatwg.org/multipage/browsers.html#determining-navigation-params-policy-container
static PolicyContainer determine_navigation_params_policy_container(URL::URL const& response_url,
    Optional<PolicyContainer> history_policy_container,
    Optional<PolicyContainer> initiator_policy_container,
    Optional<PolicyContainer> parent_policy_container,
    Optional<PolicyContainer> response_policy_container)
{
    // NOTE: The clone a policy container AO is just a C++ copy

    // 1. If historyPolicyContainer is not null, then:
    if (history_policy_container.has_value()) {
        // FIXME: 1. Assert: responseURL requires storing the policy container in history.

        // 2. Return a clone of historyPolicyContainer.
        return *history_policy_container;
    }

    // 2. If responseURL is about:srcdoc, then:
    if (response_url == "about:srcdoc"sv) {
        // 1. Assert: parentPolicyContainer is not null.
        VERIFY(parent_policy_container.has_value());

        // 2. Return a clone of parentPolicyContainer.
        return *parent_policy_container;
    }

    // 3. If responseURL is local and initiatorPolicyContainer is not null, then return a clone of initiatorPolicyContainer.
    if (Fetch::Infrastructure::is_local_url(response_url) && initiator_policy_container.has_value())
        return *initiator_policy_container;

    // 4. If responsePolicyContainer is not null, then return responsePolicyContainer.
    // FIXME: File a spec issue to say "a clone of" here for consistency
    if (response_policy_container.has_value())
        return *response_policy_container;

    // 5. Return a new policy container.
    return {};
}

// https://html.spec.whatwg.org/multipage/browsers.html#obtain-coop
static OpenerPolicy obtain_an_opener_policy(GC::Ref<Fetch::Infrastructure::Response>, Fetch::Infrastructure::Request::ReservedClientType const& reserved_client)
{

    // 1. Let policy be a new opener policy.
    OpenerPolicy policy = {};

    // AD-HOC: We don't yet setup environments in all cases
    if (!reserved_client)
        return policy;

    auto& reserved_environment = *reserved_client;

    // 2. If reservedEnvironment is a non-secure context, then return policy.
    if (is_non_secure_context(reserved_environment))
        return policy;

    // FIXME: We don't yet have the technology to extract structured data from Fetch headers
    // FIXME: 3. Let parsedItem be the result of getting a structured field value given `Cross-Origin-Opener-Policy` and "item" from response's header list.
    // FIXME: 4. If parsedItem is not null, then:
    //     FIXME: nested steps...
    // FIXME: 5. Set parsedItem to the result of getting a structured field value given `Cross-Origin-Opener-Policy-Report-Only` and "item" from response's header list.
    // FIXME: 6. If parsedItem is not null, then:
    //     FIXME: nested steps...

    // 7. Return policy.
    return policy;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#attempt-to-create-a-non-fetch-scheme-document
static GC::Ptr<DOM::Document> attempt_to_create_a_non_fetch_scheme_document(NonFetchSchemeNavigationParams const& params)
{
    // FIXME: Implement this algorithm to hand off to external software or display inline content
    dbgln("(FIXME) Don't know how to navigate to {}", params.url);
    return nullptr;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#create-navigation-params-from-a-srcdoc-resource
static WebIDL::ExceptionOr<GC::Ref<NavigationParams>> create_navigation_params_from_a_srcdoc_resource(GC::Ptr<SessionHistoryEntry> entry, GC::Ptr<Navigable> navigable, TargetSnapshotParams const& target_snapshot_params, Optional<String> navigation_id)
{
    auto& vm = navigable->vm();
    VERIFY(navigable->active_window());
    auto& realm = navigable->active_window()->realm();

    // 1. Let documentResource be entry's document state's resource.
    auto document_resource = entry->document_state()->resource();
    VERIFY(document_resource.has<String>());

    // 2. Let response be a new response with
    //    URL: about:srcdoc
    //    header list: (`Content-Type`, `text/html`)
    //    body: the UTF-8 encoding of documentResource, as a body
    auto response = Fetch::Infrastructure::Response::create(vm);
    response->url_list().append(URL::URL("about:srcdoc"));

    auto header = Fetch::Infrastructure::Header::from_string_pair("Content-Type"sv, "text/html"sv);
    response->header_list()->append(move(header));

    response->set_body(TRY(Fetch::Infrastructure::byte_sequence_as_body(realm, document_resource.get<String>().bytes())));

    // 3. Let responseOrigin be the result of determining the origin given response's URL, targetSnapshotParams's sandboxing flags, and entry's document state's origin.
    auto response_origin = determine_the_origin(response->url(), target_snapshot_params.sandboxing_flags, entry->document_state()->origin());

    // 4. Let coop be a new opener policy.
    OpenerPolicy coop = {};

    // 5. Let coopEnforcementResult be a new opener policy enforcement result with
    //    url: response's URL
    //    origin: responseOrigin
    //    opener policy: coop
    OpenerPolicyEnforcementResult coop_enforcement_result {
        .url = *response->url(),
        .origin = response_origin,
        .opener_policy = coop
    };

    // 6. Let policyContainer be the result of determining navigation params policy container given response's URL,
    //    entry's document state's history policy container, null, navigable's container document's policy container, and null.
    Optional<PolicyContainer> history_policy_container = entry->document_state()->history_policy_container().visit(
        [](PolicyContainer const& c) -> Optional<PolicyContainer> { return c; },
        [](DocumentState::Client) -> Optional<PolicyContainer> { return {}; });
    PolicyContainer policy_container;
    if (navigable->container()) {
        // NOTE: Specification assumes that only navigables corresponding to iframes can be navigated to about:srcdoc.
        //       We also use srcdoc to implement load_html() for top level navigables so we need to null check container
        //       because it might be null.
        policy_container = determine_navigation_params_policy_container(*response->url(), history_policy_container, {}, navigable->container_document()->policy_container(), {});
    }

    // 7. Return a new navigation params, with
    //    id: navigationId
    //    navigable: navigable
    //    request: null
    //    response: response
    //    fetch controller: null
    //    commit early hints: null
    //    COOP enforcement result: coopEnforcementResult
    //    reserved environment: null
    //    origin: responseOrigin
    //    policy container: policyContainer
    //    final sandboxing flag set: targetSnapshotParams's sandboxing flags
    //    opener policy: coop
    //    FIXME: navigation timing type: navTimingType
    //    about base URL: entry's document state's about base URL
    auto navigation_params = vm.heap().allocate<NavigationParams>();
    navigation_params->id = move(navigation_id);
    navigation_params->navigable = navigable;
    navigation_params->response = response;
    navigation_params->coop_enforcement_result = move(coop_enforcement_result);
    navigation_params->origin = move(response_origin);
    navigation_params->policy_container = policy_container;
    navigation_params->final_sandboxing_flag_set = target_snapshot_params.sandboxing_flags;
    navigation_params->opener_policy = move(coop);
    navigation_params->about_base_url = entry->document_state()->about_base_url();

    return navigation_params;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#create-navigation-params-by-fetching
static WebIDL::ExceptionOr<Navigable::NavigationParamsVariant> create_navigation_params_by_fetching(GC::Ptr<SessionHistoryEntry> entry, GC::Ptr<Navigable> navigable, SourceSnapshotParams const& source_snapshot_params, TargetSnapshotParams const& target_snapshot_params, CSPNavigationType csp_navigation_type, Optional<String> navigation_id)
{
    auto& vm = navigable->vm();
    VERIFY(navigable->active_window());
    auto& realm = navigable->active_window()->realm();
    auto& active_document = *navigable->active_document();

    (void)csp_navigation_type;

    // FIXME: 1. Assert: this is running in parallel.

    // 2. Let documentResource be entry's document state's resource.
    auto document_resource = entry->document_state()->resource();

    // 3. Let request be a new request, with
    //    url: entry's URL
    //    client: sourceSnapshotParams's fetch client
    //    destination: "document"
    //    credentials mode: "include"
    //    use-URL-credentials flag: set
    //    redirect mode: "manual"
    //    replaces client id: navigable's active document's relevant settings object's id
    //    mode: "navigate"
    //    referrer: entry's document state's request referrer
    //    referrer policy: entry's document state's request referrer policy
    auto request = Fetch::Infrastructure::Request::create(vm);
    request->set_url(entry->url());
    request->set_client(source_snapshot_params.fetch_client);
    request->set_destination(Fetch::Infrastructure::Request::Destination::Document);
    request->set_credentials_mode(Fetch::Infrastructure::Request::CredentialsMode::Include);
    request->set_use_url_credentials(true);
    request->set_redirect_mode(Fetch::Infrastructure::Request::RedirectMode::Manual);
    request->set_replaces_client_id(active_document.relevant_settings_object().id);
    request->set_mode(Fetch::Infrastructure::Request::Mode::Navigate);
    request->set_referrer(entry->document_state()->request_referrer());

    // 4. If documentResource is a POST resource, then:
    if (auto* post_resource = document_resource.get_pointer<POSTResource>()) {
        // 1. Set request's method to `POST`.
        request->set_method(TRY_OR_THROW_OOM(vm, ByteBuffer::copy("POST"sv.bytes())));

        // 2. Set request's body to documentResource's request body.
        request->set_body(document_resource.get<POSTResource>().request_body.value());

        // 3. Set `Content-Type` to documentResource's request content-type in request's header list.
        auto request_content_type = [&]() {
            switch (post_resource->request_content_type) {
            case POSTResource::RequestContentType::ApplicationXWWWFormUrlencoded:
                return "application/x-www-form-urlencoded"sv;
            case POSTResource::RequestContentType::MultipartFormData:
                return "multipart/form-data"sv;
            case POSTResource::RequestContentType::TextPlain:
                return "text/plain"sv;
            default:
                VERIFY_NOT_REACHED();
            }
        }();

        StringBuilder request_content_type_buffer;
        if (!post_resource->request_content_type_directives.is_empty()) {
            request_content_type_buffer.append(request_content_type);

            for (auto const& directive : post_resource->request_content_type_directives)
                request_content_type_buffer.appendff("; {}={}", directive.type, directive.value);

            request_content_type = request_content_type_buffer.string_view();
        }

        auto header = Fetch::Infrastructure::Header::from_string_pair("Content-Type"sv, request_content_type);
        request->header_list()->append(move(header));
    }

    // 5. If entry's document state's reload pending is true, then set request's reload-navigation flag.
    if (entry->document_state()->reload_pending())
        request->set_reload_navigation(true);

    // 6. Otherwise, if entry's document state's ever populated is true, then set request's history-navigation flag.
    if (entry->document_state()->ever_populated())
        request->set_history_navigation(true);

    // 7. If sourceSnapshotParams's has transient activation is true, then set request's user-activation to true.
    if (source_snapshot_params.has_transient_activation)
        request->set_user_activation(true);

    // 8. If navigable's container is non-null:
    if (navigable->container() != nullptr) {
        // 1. If the navigable's container has a browsing context scope origin, then set request's origin to that browsing context scope origin.
        // FIXME: From "browsing context scope origin": This definition is broken and needs investigation to see what it was intended to express: see issue #4703.
        //        The referenced issue suggests that it is a no-op to retrieve the browsing context scope origin.

        // 2. Set request's destination to navigable's container's local name.
        // FIXME: Are there other container types? If so, we need a helper here
        Web::Fetch::Infrastructure::Request::Destination destination = is<HTMLIFrameElement>(*navigable->container()) ? Web::Fetch::Infrastructure::Request::Destination::IFrame
                                                                                                                      : Web::Fetch::Infrastructure::Request::Destination::Object;
        request->set_destination(destination);

        // 3. If sourceSnapshotParams's fetch client is navigable's container document's relevant settings object,
        //    then set request's initiator type to navigable's container's local name.
        // NOTE: This ensure that only container-initiated navigations are reported to resource timing.
        if (source_snapshot_params.fetch_client == &navigable->container_document()->relevant_settings_object()) {
            // FIXME: Are there other container types? If so, we need a helper here
            Web::Fetch::Infrastructure::Request::InitiatorType initiator_type = is<HTMLIFrameElement>(*navigable->container()) ? Web::Fetch::Infrastructure::Request::InitiatorType::IFrame
                                                                                                                               : Web::Fetch::Infrastructure::Request::InitiatorType::Object;
            request->set_initiator_type(initiator_type);
        }
    }

    // 9. Let response be null.
    // NOTE: We use a heap-allocated cell to hold the response pointer because the processResponse callback below
    //       might use it after this stack is freed.
    auto response_holder = ResponseHolder::create(vm);

    // 10. Let responseOrigin be null.
    Optional<URL::Origin> response_origin;

    // 11. Let fetchController be null.
    GC::Ptr<Fetch::Infrastructure::FetchController> fetch_controller = nullptr;

    // 12. Let coopEnforcementResult be a new opener policy enforcement result, with
    // - url: navigable's active document's URL
    // - origin: navigable's active document's origin
    // - opener policy: navigable's active document's opener policy
    // - current context is navigation source: true if navigable's active document's origin is same origin with
    //                                         entry's document state's initiator origin otherwise false
    OpenerPolicyEnforcementResult coop_enforcement_result = {
        .url = active_document.url(),
        .origin = active_document.origin(),
        .opener_policy = active_document.opener_policy(),
        .current_context_is_navigation_source = entry->document_state()->initiator_origin().has_value() && active_document.origin().is_same_origin(*entry->document_state()->initiator_origin())
    };

    // 13. Let finalSandboxFlags be an empty sandboxing flag set.
    SandboxingFlagSet final_sandbox_flags = {};

    // 14. Let responsePolicyContainer be null.
    Optional<PolicyContainer> response_policy_container = {};

    // 15. Let responseCOOP be a new opener policy.
    OpenerPolicy response_coop = {};

    // 16. Let locationURL be null.
    ErrorOr<Optional<URL::URL>> location_url { OptionalNone {} };

    // 17. Let currentURL be request's current URL.
    URL::URL current_url = request->current_url();

    // 18. Let commitEarlyHints be null.
    Function<void(DOM::Document&)> commit_early_hints = nullptr;

    // 19. While true:
    while (true) {
        // FIXME: 1. If request's reserved client is not null and currentURL's origin is not the same as request's reserved client's creation URL's origin, then:
        // FIXME: 2. If request's reserved client is null, then:
        // FIXME: 3. If the result of should navigation request of type be blocked by Content Security Policy? given request and cspNavigationType is "Blocked", then set response to a network error and break. [CSP]

        // 4. Set response to null.
        response_holder->set_response(nullptr);

        // 5. If fetchController is null, then set fetchController to the result of fetching request,
        //    with processEarlyHintsResponse set to processEarlyHintsResponseas defined below, processResponse
        //    set to processResponse as defined below, and useParallelQueue set to true.
        if (!fetch_controller) {
            // FIXME: Let processEarlyHintsResponse be the following algorithm given a response earlyResponse:

            // Let processResponse be the following algorithm given a response fetchedResponse:
            auto process_response = [response_holder](GC::Ref<Fetch::Infrastructure::Response> fetch_response) {
                // 1. Set response to fetchedResponse.
                response_holder->set_response(fetch_response);
            };

            fetch_controller = TRY(Fetch::Fetching::fetch(
                realm,
                request,
                Fetch::Infrastructure::FetchAlgorithms::create(vm,
                    {
                        .process_request_body_chunk_length = {},
                        .process_request_end_of_body = {},
                        .process_early_hints_response = {},
                        .process_response = move(process_response),
                        .process_response_end_of_body = {},
                        .process_response_consume_body = {},
                    }),
                Fetch::Fetching::UseParallelQueue::Yes));
        }
        // 6. Otherwise, process the next manual redirect for fetchController.
        else {
            fetch_controller->process_next_manual_redirect();
        }

        // 7. Wait until either response is non-null, or navigable's ongoing navigation changes to no longer equal navigationId.
        HTML::main_thread_event_loop().spin_until(GC::create_function(vm.heap(), [&]() {
            if (response_holder->response() != nullptr)
                return true;

            if (navigation_id.has_value() && (!navigable->ongoing_navigation().has<String>() || navigable->ongoing_navigation().get<String>() != *navigation_id))
                return true;

            return false;
        }));
        // If the latter condition occurs, then abort fetchController, and return. Otherwise, proceed onward.
        if (navigation_id.has_value() && (!navigable->ongoing_navigation().has<String>() || navigable->ongoing_navigation().get<String>() != *navigation_id)) {
            fetch_controller->abort(realm, {});
            return Empty {};
        }

        // 8. If request's body is null, then set entry's document state's resource to null.
        if (!request->body().has<Empty>()) {
            entry->document_state()->set_resource(Empty {});
        }

        // FIXME 9. Set responsePolicyContainer to the result of creating a policy container from a fetch response given response and request's reserved client.
        // FIXME 10. Set finalSandboxFlags to the union of targetSnapshotParams's sandboxing flags and responsePolicyContainer's CSP list's CSP-derived sandboxing flags.

        // 11. Set responseOrigin to the result of determining the origin given response's URL, finalSandboxFlags, and entry's document state's initiator origin.
        response_origin = determine_the_origin(response_holder->response()->url(), final_sandbox_flags, entry->document_state()->initiator_origin());

        // 12. If navigable is a top-level traversable, then:
        if (navigable->is_top_level_traversable()) {
            // 1. Set responseCOOP to the result of obtaining an opener policy given response and request's reserved client.
            response_coop = obtain_an_opener_policy(*response_holder->response(), request->reserved_client());

            // FIXME: 2. Set coopEnforcementResult to the result of enforcing the response's opener policy given navigable's active browsing context,
            //    response's URL, responseOrigin, responseCOOP, coopEnforcementResult and request's referrer.

            // FIXME: 3. If finalSandboxFlags is not empty and responseCOOP's value is not "unsafe-none", then set response to an appropriate network error and break.
            // NOTE: This results in a network error as one cannot simultaneously provide a clean slate to a response
            //       using opener policy and sandbox the result of navigating to that response.
        }

        // 13. FIXME If response is not a network error, navigable is a child navigable, and the result of performing a cross-origin resource policy check
        //    with navigable's container document's origin, navigable's container document's relevant settings object, request's destination, response,
        //    and true is blocked, then set response to a network error and break.
        // NOTE: Here we're running the cross-origin resource policy check against the parent navigable rather than navigable itself
        //       This is because we care about the same-originness of the embedded content against the parent context, not the navigation source.

        // 14. Set locationURL to response's location URL given currentURL's fragment.
        location_url = response_holder->response()->location_url(current_url.fragment());

        VERIFY(!location_url.is_error());

        // 15. If locationURL is failure or null, then break.
        if (location_url.is_error() || !location_url.value().has_value()) {
            break;
        }

        // 16. Assert: locationURL is a URL.
        VERIFY(location_url.value()->is_valid());

        // 17. Set entry's classic history API state to StructuredSerializeForStorage(null).
        entry->set_classic_history_api_state(MUST(structured_serialize_for_storage(vm, JS::js_null())));

        // 18. Let oldDocState be entry's document state.
        auto old_doc_state = entry->document_state();

        // 19. Set entry's document state to a new document state, with
        // history policy container: a clone of the oldDocState's history policy container if it is non-null; null otherwise
        // request referrer: oldDocState's request referrer
        // request referrer policy: oldDocState's request referrer policy
        // origin: oldDocState's origin
        // resource: oldDocState's resource
        // ever populated: oldDocState's ever populated
        // navigable target name: oldDocState's navigable target name
        auto new_document_state = navigable->heap().allocate<DocumentState>();
        new_document_state->set_history_policy_container(old_doc_state->history_policy_container());
        new_document_state->set_request_referrer(old_doc_state->request_referrer());
        new_document_state->set_request_referrer_policy(old_doc_state->request_referrer_policy());
        new_document_state->set_origin(old_doc_state->origin());
        new_document_state->set_resource(old_doc_state->resource());
        new_document_state->set_ever_populated(old_doc_state->ever_populated());
        new_document_state->set_navigable_target_name(old_doc_state->navigable_target_name());
        entry->set_document_state(new_document_state);

        // 20. If locationURL's scheme is not an HTTP(S) scheme, then:
        if (!Fetch::Infrastructure::is_http_or_https_scheme(location_url.value()->scheme())) {
            // 1. Set entry's document state's resource to null.
            entry->document_state()->set_resource(Empty {});

            // 2. Break.
            break;
        }

        // 21. Set currentURL to locationURL.
        current_url = location_url.value().value();

        // 22. Set entry's URL to currentURL.
        entry->set_url(current_url);
    }

    // 20. If locationURL is a URL whose scheme is not a fetch scheme, then return a new non-fetch scheme navigation params, with
    if (!location_url.is_error() && location_url.value().has_value() && !Fetch::Infrastructure::is_fetch_scheme(location_url.value().value().scheme())) {
        // - id: navigationId
        // - navigable: navigable
        // - URL: locationURL
        // - target snapshot sandboxing flags: targetSnapshotParams's sandboxing flags
        // - source snapshot has transient activation: sourceSnapshotParams's has transient activation
        // - initiator origin: responseOrigin
        // FIXME: - navigation timing type: navTimingType
        auto navigation_params = vm.heap().allocate<NonFetchSchemeNavigationParams>();
        navigation_params->id = navigation_id;
        navigation_params->navigable = navigable;
        navigation_params->url = location_url.release_value().value();
        navigation_params->target_snapshot_sandboxing_flags = target_snapshot_params.sandboxing_flags;
        navigation_params->source_snapshot_has_transient_activation = source_snapshot_params.has_transient_activation;
        navigation_params->initiator_origin = move(*response_origin);
        return navigation_params;
    }

    // 21. If any of the following are true:
    //       - response is a network error;
    //       - locationURL is failure; or
    //       - locationURL is a URL whose scheme is a fetch scheme
    //     then return null.
    if (response_holder->response()->is_network_error()) {
        // AD-HOC: We pass the error message if we have one in NullWithError
        if (response_holder->response()->network_error_message().has_value() && !response_holder->response()->network_error_message().value().is_null())
            return response_holder->response()->network_error_message().value();
        else
            return Empty {};
    } else if (location_url.is_error() || (location_url.value().has_value() && Fetch::Infrastructure::is_fetch_scheme(location_url.value().value().scheme())))
        return Empty {};

    // 22. Assert: locationURL is null and response is not a network error.
    VERIFY(!location_url.value().has_value());
    VERIFY(!response_holder->response()->is_network_error());

    // 23. Let resultPolicyContainer be the result of determining navigation params policy container given response's URL,
    //     entry's document state's history policy container, sourceSnapshotParams's source policy container, null, and responsePolicyContainer.
    Optional<PolicyContainer> history_policy_container = entry->document_state()->history_policy_container().visit(
        [](PolicyContainer const& c) -> Optional<PolicyContainer> { return c; },
        [](DocumentState::Client) -> Optional<PolicyContainer> { return {}; });
    auto result_policy_container = determine_navigation_params_policy_container(*response_holder->response()->url(), history_policy_container, source_snapshot_params.source_policy_container, {}, response_policy_container);

    // 24. If navigable's container is an iframe, and response's timing allow passed flag is set, then set container's pending resource-timing start time to null.
    if (navigable->container() && is<HTML::HTMLIFrameElement>(*navigable->container()) && response_holder->response()->timing_allow_passed())
        static_cast<HTML::HTMLIFrameElement&>(*navigable->container()).set_pending_resource_start_time({});

    // 25. Return a new navigation params, with
    //     id: navigationId
    //     navigable: navigable
    //     request: request
    //     response: response
    //     fetch controller: fetchController
    //     commit early hints: commitEarlyHints
    //     opener policy: responseCOOP
    //     reserved environment: request's reserved client
    //     origin: responseOrigin
    //     policy container: resultPolicyContainer
    //     final sandboxing flag set: finalSandboxFlags
    //     COOP enforcement result: coopEnforcementResult
    //     FIXME: navigation timing type: navTimingType
    //     about base URL: entry's document state's about base URL
    auto navigation_params = vm.heap().allocate<NavigationParams>();
    navigation_params->id = navigation_id;
    navigation_params->navigable = navigable;
    navigation_params->request = request;
    navigation_params->response = *response_holder->response();
    navigation_params->fetch_controller = fetch_controller;
    navigation_params->commit_early_hints = move(commit_early_hints);
    navigation_params->coop_enforcement_result = coop_enforcement_result;
    navigation_params->reserved_environment = request->reserved_client();
    navigation_params->origin = *response_origin;
    navigation_params->policy_container = result_policy_container;
    navigation_params->final_sandboxing_flag_set = final_sandbox_flags;
    navigation_params->opener_policy = response_coop;
    navigation_params->about_base_url = entry->document_state()->about_base_url();
    return navigation_params;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#attempt-to-populate-the-history-entry's-document
WebIDL::ExceptionOr<void> Navigable::populate_session_history_entry_document(
    GC::Ptr<SessionHistoryEntry> entry,
    SourceSnapshotParams const& source_snapshot_params,
    TargetSnapshotParams const& target_snapshot_params,
    Optional<String> navigation_id,
    Navigable::NavigationParamsVariant navigation_params,
    CSPNavigationType csp_navigation_type,
    bool allow_POST,
    GC::Ptr<GC::Function<void()>> completion_steps)
{
    // AD-HOC: Not in the spec but subsequent steps will fail if the navigable doesn't have an active window.
    if (!active_window())
        return {};

    // FIXME: 1. Assert: this is running in parallel.

    // 2. Assert: if navigationParams is non-null, then navigationParams's response is non-null.
    if (!navigation_params.has<Empty>() && !navigation_params.has<NullWithError>())
        VERIFY(navigation_params.has<GC::Ref<NavigationParams>>() && navigation_params.get<GC::Ref<NavigationParams>>()->response);

    // 3. Let currentBrowsingContext be navigable's active browsing context.
    [[maybe_unused]] auto current_browsing_context = active_browsing_context();

    // 4. Let documentResource be entry's document state's resource.
    auto document_resource = entry->document_state()->resource();

    // 5. If navigationParams is null, then:
    if (navigation_params.has<Empty>() || navigation_params.has<NullWithError>()) {
        // 1. If documentResource is a string, then set navigationParams to the result
        //    of creating navigation params from a srcdoc resource given entry, navigable,
        //    targetSnapshotParams, navigationId, and navTimingType.
        if (document_resource.has<String>()) {
            navigation_params = TRY(create_navigation_params_from_a_srcdoc_resource(entry, this, target_snapshot_params, navigation_id));
        }
        // 2. Otherwise, if all of the following are true:
        //    - entry's URL's scheme is a fetch scheme; and
        //    - documentResource is null, or allowPOST is true and documentResource's request body is not failure (FIXME: check if request body is not failure)
        // then set navigationParams to the result of creating navigation params by fetching given entry, navigable, sourceSnapshotParams, targetSnapshotParams, cspNavigationType, navigationId, and navTimingType.
        else if (Fetch::Infrastructure::is_fetch_scheme(entry->url().scheme()) && (document_resource.has<Empty>() || allow_POST)) {
            navigation_params = TRY(create_navigation_params_by_fetching(entry, this, source_snapshot_params, target_snapshot_params, csp_navigation_type, navigation_id));
        }
        // 3. Otherwise, if entry's URL's scheme is not a fetch scheme, then set navigationParams to a new non-fetch scheme navigation params, with:
        else if (!Fetch::Infrastructure::is_fetch_scheme(entry->url().scheme())) {
            // - id: navigationId
            // - navigable: navigable
            // - URL: entry's URL
            // - target snapshot sandboxing flags: targetSnapshotParams's sandboxing flags
            // - source snapshot has transient activation: sourceSnapshotParams's has transient activation
            // - initiator origin: entry's document state's initiator origin
            // FIXME: - navigation timing type: navTimingType
            auto non_fetching_scheme_navigation_params = vm().heap().allocate<NonFetchSchemeNavigationParams>();
            non_fetching_scheme_navigation_params->id = navigation_id;
            non_fetching_scheme_navigation_params->navigable = this;
            non_fetching_scheme_navigation_params->url = entry->url();
            non_fetching_scheme_navigation_params->target_snapshot_sandboxing_flags = target_snapshot_params.sandboxing_flags;
            non_fetching_scheme_navigation_params->source_snapshot_has_transient_activation = source_snapshot_params.has_transient_activation;
            non_fetching_scheme_navigation_params->initiator_origin = *entry->document_state()->initiator_origin();
            navigation_params = non_fetching_scheme_navigation_params;
        }
    }

    // AD-HOC: Not in the spec but subsequent steps will fail if the navigable doesn't have an active window.
    if (!active_window())
        return {};

    // 6. Queue a global task on the navigation and traversal task source, given navigable's active window, to run these steps:
    queue_global_task(Task::Source::NavigationAndTraversal, *active_window(), GC::create_function(heap(), [this, entry, navigation_params = move(navigation_params), navigation_id, completion_steps]() mutable {
        // NOTE: This check is not in the spec but we should not continue navigation if navigable has been destroyed.
        if (has_been_destroyed())
            return;

        // 1. If navigable's ongoing navigation no longer equals navigationId, then run completionSteps and abort these steps.
        if (navigation_id.has_value() && (!ongoing_navigation().has<String>() || ongoing_navigation().get<String>() != *navigation_id)) {
            if (completion_steps)
                completion_steps->function()();
            return;
        }

        // 2. Let saveExtraDocumentState be true.
        auto saveExtraDocumentState = true;

        // 3. If navigationParams is a non-fetch scheme navigation params, then:
        if (navigation_params.has<GC::Ref<NonFetchSchemeNavigationParams>>()) {
            // 1. Set entry's document state's document to the result of running attempt to create a non-fetch scheme document given navigationParams.
            entry->document_state()->set_document(attempt_to_create_a_non_fetch_scheme_document(navigation_params.get<GC::Ref<NonFetchSchemeNavigationParams>>()));
            if (entry->document()) {
                entry->document_state()->set_ever_populated(true);
            }

            // 2. Set saveExtraDocumentState to false.
            saveExtraDocumentState = false;
        }

        // 4. Otherwise, if any of the following are true:
        //  - navigationParams is null;
        //  - FIXME: the result of should navigation response to navigation request of type in target be blocked by Content Security Policy? given navigationParams's request, navigationParams's response, navigationParams's policy container's CSP list, cspNavigationType, and navigable is "Blocked";
        //  - FIXME: navigationParams's reserved environment is non-null and the result of checking a navigation response's adherence to its embedder policy given navigationParams's response, navigable, and navigationParams's policy container's embedder policy is false; or
        //  - FIXME: the result of checking a navigation response's adherence to `X-Frame-Options` given navigationParams's response, navigable, navigationParams's policy container's CSP list, and navigationParams's origin is false,
        if (navigation_params.has<Empty>() || navigation_params.has<NullWithError>()) {
            // 1. Set entry's document state's document to the result of creating a document for inline content that doesn't have a DOM, given navigable, null, and navTimingType. The inline content should indicate to the user the sort of error that occurred.
            auto error_message = navigation_params.has<NullWithError>() ? navigation_params.get<NullWithError>() : "Unknown error"sv;

            auto error_html = load_error_page(entry->url(), error_message).release_value_but_fixme_should_propagate_errors();
            entry->document_state()->set_document(create_document_for_inline_content(this, navigation_id, [error_html](auto& document) {
                auto parser = HTML::HTMLParser::create(document, error_html, "utf-8"sv);
                document.set_url(URL::URL("about:error"));
                parser->run();
            }));

            // 2. Make document unsalvageable given entry's document state's document and "navigation-failure".
            entry->document()->make_unsalvageable("navigation-failure"_string);

            // 3. Set saveExtraDocumentState to false.
            saveExtraDocumentState = false;

            // 4. If navigationParams is not null, then:
            if (!navigation_params.has<Empty>() && !navigation_params.has<NullWithError>()) {
                // FIXME: 1. Run the environment discarding steps for navigationParams's reserved environment.
                // FIXME: 2. Invoke WebDriver BiDi navigation failed with currentBrowsingContext and a new WebDriver BiDi navigation status whose id is navigationId, status is "canceled", and url is navigationParams's response's URL.
            }
        }
        // FIXME: 5. Otherwise, if navigationParams's response has a `Content-Disposition`
        //            header specifying the attachment disposition type, then:
        // 6. Otherwise, if navigationParams's response's status is not 204 and is not 205, then set entry's document state's document to the result of
        //    loading a document given navigationParams, sourceSnapshotParams, and entry's document state's initiator origin.
        else if (auto const& response = navigation_params.get<GC::Ref<NavigationParams>>()->response; response->status() != 204 && response->status() != 205) {
            auto document = load_document(navigation_params.get<GC::Ref<NavigationParams>>());
            entry->document_state()->set_document(document);
        }

        // 7. If entry's document state's document is not null, then:
        if (entry->document()) {
            // 1. Set entry's document state's ever populated to true.
            entry->document_state()->set_ever_populated(true);

            // 2. If saveExtraDocumentState is true:
            if (saveExtraDocumentState) {
                // 1. Let document be entry's document state's document.
                auto document = entry->document();

                // 2. Set entry's document state's origin to document's origin.
                entry->document_state()->set_origin(document->origin());

                // FIXME: 3. If document's URL requires storing the policy container in history, then:
            }

            // 3. If entry's document state's request referrer is "client", and navigationParams is a navigation params (i.e., neither null nor a non-fetch scheme navigation params), then:
            if (entry->document_state()->request_referrer() == Fetch::Infrastructure::Request::Referrer::Client
                && (!navigation_params.has<Empty>() && !navigation_params.has<NullWithError>() && navigation_params.has<GC::Ref<NonFetchSchemeNavigationParams>>())) {
                // 1. Assert: navigationParams's request is not null.
                VERIFY(navigation_params.has<GC::Ref<NavigationParams>>() && navigation_params.get<GC::Ref<NavigationParams>>()->request);

                // 2. Set entry's document state's request referrer to navigationParams's request's referrer.
                entry->document_state()->set_request_referrer(navigation_params.get<GC::Ref<NavigationParams>>()->request->referrer());
            }
        }

        // 8. Run completionSteps.
        if (completion_steps)
            completion_steps->function()();
    }));

    return {};
}

// To navigate a navigable navigable to a URL url using a Document sourceDocument,
// with an optional POST resource, string, or null documentResource (default null),
// an optional response-or-null response (default null), an optional boolean exceptionsEnabled (default false),
// an optional NavigationHistoryBehavior historyHandling (default "auto"),
// an optional serialized state-or-null navigationAPIState (default null),
// an optional entry list or null formDataEntryList (default null),
// an optional referrer policy referrerPolicy (default the empty string),
// and an optional user navigation involvement userInvolvement (default "none"):

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#navigate
WebIDL::ExceptionOr<void> Navigable::navigate(NavigateParams params)
{
    // AD-HOC: Not in the spec but subsequent steps will fail if the navigable doesn't have an active window.
    if (!active_window())
        return {};

    auto const& url = params.url;
    auto source_document = params.source_document;
    auto const& document_resource = params.document_resource;
    auto response = params.response;
    auto exceptions_enabled = params.exceptions_enabled;
    auto history_handling = params.history_handling;
    auto const& navigation_api_state = params.navigation_api_state;
    auto const& form_data_entry_list = params.form_data_entry_list;
    auto referrer_policy = params.referrer_policy;
    auto user_involvement = params.user_involvement;
    auto& active_document = *this->active_document();
    auto& realm = active_document.realm();
    auto& vm = this->vm();

    // 1. Let cspNavigationType be "form-submission" if formDataEntryList is non-null; otherwise "other".
    auto csp_navigation_type = form_data_entry_list.has_value() ? CSPNavigationType::FormSubmission : CSPNavigationType::Other;

    // 2. Let sourceSnapshotParams be the result of snapshotting source snapshot params given sourceDocument.
    auto source_snapshot_params = source_document->snapshot_source_snapshot_params();

    // 3. Let initiatorOriginSnapshot be sourceDocument's origin.
    auto initiator_origin_snapshot = source_document->origin();

    // 4. Let initiatorBaseURLSnapshot be sourceDocument's document base URL.
    auto initiator_base_url_snapshot = source_document->base_url();

    // 5. If sourceDocument's node navigable is not allowed by sandboxing to navigate navigable given and sourceSnapshotParams, then:
    if (!source_document->navigable()->allowed_by_sandboxing_to_navigate(*this, source_snapshot_params)) {
        // 1. If exceptionsEnabled is true, then throw a "SecurityError" DOMException.
        if (exceptions_enabled) {
            return WebIDL::SecurityError::create(realm, "Source document's node navigable is not allowed to navigate"_string);
        }

        // 2 Return.
        return {};
    }

    // 6. Let navigationId be the result of generating a random UUID.
    String navigation_id = TRY_OR_THROW_OOM(vm, Crypto::generate_random_uuid());

    // FIXME: 7. If the surrounding agent is equal to navigable's active document's relevant agent, then continue these steps.
    //           Otherwise, queue a global task on the navigation and traversal task source given navigable's active window to continue these steps.

    // 8. If navigable's active document's unload counter is greater than 0,
    //    then invoke WebDriver BiDi navigation failed with a WebDriver BiDi navigation status whose id is navigationId,
    //    status is "canceled", and url is url, and return.
    if (active_document.unload_counter() > 0) {
        // FIXME: invoke WebDriver BiDi navigation failed with a WebDriver BiDi navigation status whose id is navigationId,
        //        status is "canceled", and url is url
        return {};
    }

    // 9. If historyHandling is "auto", then:
    if (history_handling == Bindings::NavigationHistoryBehavior::Auto) {
        // FIXME: Fix spec typo targetNavigable --> navigable
        // 1. If url equals navigable's active document's URL,
        //     and initiatorOriginSnapshot is same origin with targetNavigable's active document's origin,
        //     then set historyHandling to "replace".
        if (url == active_document.url() && initiator_origin_snapshot.is_same_origin(active_document.origin()))
            history_handling = Bindings::NavigationHistoryBehavior::Replace;

        // 2. Otherwise, set historyHandling to "push".
        else
            history_handling = Bindings::NavigationHistoryBehavior::Push;
    }

    // 10. If the navigation must be a replace given url and navigable's active document, then set historyHandling to "replace".
    if (navigation_must_be_a_replace(url, active_document))
        history_handling = Bindings::NavigationHistoryBehavior::Replace;

    // 11. If all of the following are true:
    //    - documentResource is null;
    //    - response is null;
    //    - url equals navigable's active session history entry's URL with exclude fragments set to true; and
    //    - url's fragment is non-null
    if (document_resource.has<Empty>()
        && !response
        && url.equals(active_session_history_entry()->url(), URL::ExcludeFragment::Yes)
        && url.fragment().has_value()) {
        // 1. Navigate to a fragment given navigable, url, historyHandling, userInvolvement, navigationAPIState, and navigationId.
        TRY(navigate_to_a_fragment(url, to_history_handling_behavior(history_handling), user_involvement, navigation_api_state, navigation_id));

        // 2. Return.
        return {};
    }

    // 12. If navigable's parent is non-null, then set navigable's is delaying load events to true.
    if (parent() != nullptr)
        set_delaying_load_events(true);

    // 13. Let targetBrowsingContext be navigable's active browsing context.
    [[maybe_unused]] auto target_browsing_context = active_browsing_context();

    // 14. Let targetSnapshotParams be the result of snapshotting target snapshot params given navigable.
    auto target_snapshot_params = snapshot_target_snapshot_params();

    // FIXME: 15. Invoke WebDriver BiDi navigation started with targetBrowsingContext, and a new WebDriver BiDi navigation status whose id is navigationId, url is url, and status is "pending".

    // 16. If navigable's ongoing navigation is "traversal", then:
    if (ongoing_navigation().has<Traversal>()) {
        // FIXME: 1. Invoke WebDriver BiDi navigation failed with targetBrowsingContext and a new WebDriver BiDi navigation status whose id is navigationId, status is "canceled", and url is url.

        // 2. Return.
        return {};
    }

    // 17. Set navigable's ongoing navigation to navigationId.
    set_ongoing_navigation(navigation_id);

    // 18. If url's scheme is "javascript", then:
    if (url.scheme() == "javascript"sv) {
        // 1. Queue a global task on the navigation and traversal task source given navigable's active window to navigate to a javascript: URL given navigable, url, historyHandling, initiatorOriginSnapshot, and cspNavigationType.
        VERIFY(active_window());
        queue_global_task(Task::Source::NavigationAndTraversal, *active_window(), GC::create_function(heap(), [this, url, history_handling, initiator_origin_snapshot, csp_navigation_type, navigation_id] {
            (void)navigate_to_a_javascript_url(url, to_history_handling_behavior(history_handling), initiator_origin_snapshot, csp_navigation_type, navigation_id);
        }));

        // 2. Return.
        return {};
    }

    // 19. If all of the following are true:
    //     - userInvolvement is not "browser UI";
    //     - navigable's active document's origin is same origin-domain with sourceDocument's origin;
    //     - navigable's active document's is initial about:blank is false; and
    //     - url's scheme is a fetch scheme
    //     then:
    if (user_involvement != UserNavigationInvolvement::BrowserUI && active_document.origin().is_same_origin_domain(source_document->origin()) && !active_document.is_initial_about_blank() && Fetch::Infrastructure::is_fetch_scheme(url.scheme())) {
        // 1. Let navigation be navigable's active window's navigation API.
        VERIFY(active_window());
        auto navigation = active_window()->navigation();

        // 2. Let entryListForFiring be formDataEntryList if documentResource is a POST resource; otherwise, null.
        auto entry_list_for_firing = [&]() -> Optional<Vector<XHR::FormDataEntry>&> {
            if (document_resource.has<POSTResource>())
                return form_data_entry_list;
            return {};
        }();

        // 3. Let navigationAPIStateForFiring be navigationAPIState if navigationAPIState is not null;
        //    otherwise, StructuredSerializeForStorage(undefined).
        auto navigation_api_state_for_firing = navigation_api_state.value_or(MUST(structured_serialize_for_storage(vm, JS::js_undefined())));

        // FIXME: 4. Let continue be the result of firing a push/replace/reload navigate event at navigation
        //           with navigationType set to historyHandling, isSameDocument set to false, userInvolvement set to userInvolvement,
        //           formDataEntryList set to entryListForFiring, destinationURL set to url, and navigationAPIState set to navigationAPIStateForFiring.
        (void)navigation;
        (void)entry_list_for_firing;
        (void)navigation_api_state_for_firing;

        // FIXME: 5. If continue is false, then return.
    }

    if (is_top_level_traversable()) {
        active_browsing_context()->page().client().page_did_start_loading(url, false);
    }

    // 20. In parallel, run these steps:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(heap(), [this, source_snapshot_params, target_snapshot_params, csp_navigation_type, document_resource, url, navigation_id, referrer_policy, initiator_origin_snapshot, response, history_handling, initiator_base_url_snapshot] {
        // AD-HOC: Not in the spec but subsequent steps will fail if the navigable doesn't have an active window.
        if (!active_window()) {
            set_delaying_load_events(false);
            return;
        }

        // 1. Let unloadPromptCanceled be the result of checking if unloading is user-canceled for navigable's active document's inclusive descendant navigables.
        auto unload_prompt_canceled = traversable_navigable()->check_if_unloading_is_canceled(this->active_document()->inclusive_descendant_navigables());

        // 2. If unloadPromptCanceled is true, or navigable's ongoing navigation is no longer navigationId, then:
        if (unload_prompt_canceled != TraversableNavigable::CheckIfUnloadingIsCanceledResult::Continue || !ongoing_navigation().has<String>() || ongoing_navigation().get<String>() != navigation_id) {
            // FIXME: 1. Invoke WebDriver BiDi navigation failed with targetBrowsingContext and a new WebDriver BiDi navigation status whose id is navigationId, status is "canceled", and url is url.

            // 2. Abort these steps.
            set_delaying_load_events(false);
            return;
        }

        // AD-HOC: Not in the spec but subsequent steps will fail if the navigable doesn't have an active window.
        if (!active_window()) {
            set_delaying_load_events(false);
            return;
        }

        // 3. Queue a global task on the navigation and traversal task source given navigable's active window to abort a document and its descendants given navigable's active document.
        queue_global_task(Task::Source::NavigationAndTraversal, *active_window(), GC::create_function(heap(), [this] {
            VERIFY(this->active_document());
            this->active_document()->abort_a_document_and_its_descendants();
        }));

        // 4. Let documentState be a new document state with
        //    request referrer policy: referrerPolicy
        //    initiator origin: initiatorOriginSnapshot
        //    resource: documentResource
        //    navigable target name: navigable's target name
        GC::Ref<DocumentState> document_state = *heap().allocate<DocumentState>();
        document_state->set_request_referrer_policy(referrer_policy);
        document_state->set_initiator_origin(initiator_origin_snapshot);
        document_state->set_resource(document_resource);
        document_state->set_navigable_target_name(target_name());

        // 5. If url matches about:blank or is about:srcdoc, then set documentState's origin to documentState's initiator origin.
        if (url_matches_about_blank(url) || url_matches_about_srcdoc(url)) {
            // document_resource cannot have an Empty if the url is about:srcdoc since we rely on document_resource
            // having a String to call create_navigation_params_from_a_srcdoc_resource
            if (url_matches_about_srcdoc(url) && document_resource.has<Empty>()) {
                document_state->set_resource({ String {} });
            }
            // 1. Set documentState's origin to initiatorOriginSnapshot.
            document_state->set_origin(document_state->initiator_origin());

            // 2. Set documentState's about base URL to initiatorBaseURLSnapshot.
            document_state->set_about_base_url(initiator_base_url_snapshot);
        }

        // 6. Let historyEntry be a new session history entry, with its URL set to url and its document state set to documentState.
        GC::Ref<SessionHistoryEntry> history_entry = *heap().allocate<SessionHistoryEntry>();
        history_entry->set_url(url);
        history_entry->set_document_state(document_state);

        // 7. Let navigationParams be null.
        NavigationParamsVariant navigation_params = Empty {};

        // FIXME: 8. If response is non-null:
        if (response) {
        }

        // 9. Attempt to populate the history entry's document
        //     for historyEntry, given navigable, "navigate", sourceSnapshotParams,
        //     targetSnapshotParams, navigationId, navigationParams, cspNavigationType, with allowPOST
        //     set to true and completionSteps set to the following step:
        populate_session_history_entry_document(history_entry, source_snapshot_params, target_snapshot_params, navigation_id, navigation_params, csp_navigation_type, true, GC::create_function(heap(), [this, history_entry, history_handling, navigation_id] {
            // 1.     Append session history traversal steps to navigable's traversable to finalize a cross-document navigation given navigable, historyHandling, and historyEntry.
            traversable_navigable()->append_session_history_traversal_steps(GC::create_function(heap(), [this, history_entry, history_handling, navigation_id] {
                if (this->has_been_destroyed()) {
                    // NOTE: This check is not in the spec but we should not continue navigation if navigable has been destroyed.
                    set_delaying_load_events(false);
                    return;
                }
                if (this->ongoing_navigation() != navigation_id) {
                    // NOTE: This check is not in the spec but we should not continue navigation if ongoing navigation id has changed.
                    set_delaying_load_events(false);
                    return;
                }
                finalize_a_cross_document_navigation(*this, to_history_handling_behavior(history_handling), history_entry);
            }));
        })).release_value_but_fixme_should_propagate_errors();
    }));

    return {};
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#navigate-fragid
WebIDL::ExceptionOr<void> Navigable::navigate_to_a_fragment(URL::URL const& url, HistoryHandlingBehavior history_handling, UserNavigationInvolvement user_involvement, Optional<SerializationRecord> navigation_api_state, String navigation_id)
{
    (void)navigation_id;

    // 1. Let navigation be navigable's active window's navigation API.
    VERIFY(active_window());
    auto navigation = active_window()->navigation();

    // 2. Let destinationNavigationAPIState be navigable's active session history entry's navigation API state.
    // 3. If navigationAPIState is not null, then set destinationNavigationAPIState to navigationAPIState.
    auto destination_navigation_api_state = navigation_api_state.has_value() ? *navigation_api_state : active_session_history_entry()->navigation_api_state();

    // 4. Let continue be the result of firing a push/replace/reload navigate event at navigation with navigationType set to historyHandling, isSameDocument set to true,
    //    userInvolvement set to userInvolvement, and destinationURL set to url, and navigationAPIState set to destinationNavigationAPIState.
    auto navigation_type = history_handling == HistoryHandlingBehavior::Push ? Bindings::NavigationType::Push : Bindings::NavigationType::Replace;
    bool const continue_ = navigation->fire_a_push_replace_reload_navigate_event(navigation_type, url, true, user_involvement, {}, destination_navigation_api_state);

    // 5. If continue is false, then return.
    if (!continue_)
        return {};

    // 6. Let historyEntry be a new session history entry, with
    //      URL: url
    //      document state: navigable's active session history entry's document state
    //      navigation API state: destinationNavigationAPIState
    //      scroll restoration mode: navigable's active session history entry's scroll restoration mode
    GC::Ref<SessionHistoryEntry> history_entry = heap().allocate<SessionHistoryEntry>();
    history_entry->set_url(url);
    history_entry->set_document_state(active_session_history_entry()->document_state());
    history_entry->set_navigation_api_state(destination_navigation_api_state);
    history_entry->set_scroll_restoration_mode(active_session_history_entry()->scroll_restoration_mode());

    // 7. Let entryToReplace be navigable's active session history entry if historyHandling is "replace", otherwise null.
    auto entry_to_replace = history_handling == HistoryHandlingBehavior::Replace ? active_session_history_entry() : nullptr;

    // 8. Let history be navigable's active document's history object.
    auto history = active_document()->history();

    // 9. Let scriptHistoryIndex be history's index.
    auto script_history_index = history->m_index;

    // 10. Let scriptHistoryLength be history's length.
    auto script_history_length = history->m_length;

    // 11. If historyHandling is "push", then:
    if (history_handling == HistoryHandlingBehavior::Push) {
        // 1. Set history's state to null.
        history->set_state(JS::js_null());

        // 2. Increment scriptHistoryIndex.
        script_history_index++;

        // 3. Set scriptHistoryLength to scriptHistoryIndex + 1.
        script_history_length = script_history_index + 1;
    }

    // 12. Set navigable's active session history entry to historyEntry.
    m_active_session_history_entry = history_entry;

    // 13. Update document for history step application given navigable's active document, historyEntry, true, scriptHistoryIndex, and scriptHistoryLength.
    // AD HOC: Skip updating the navigation api entries twice here
    active_document()->update_for_history_step_application(*history_entry, true, script_history_length, script_history_index, navigation_type, {}, {}, false);

    // 14. Update the navigation API entries for a same-document navigation given navigation, historyEntry, and historyHandling.
    navigation->update_the_navigation_api_entries_for_a_same_document_navigation(history_entry, navigation_type);

    // 15. Scroll to the fragment given navigable's active document.
    // FIXME: Specification doesn't say when document url needs to update during fragment navigation
    active_document()->set_url(url);
    active_document()->scroll_to_the_fragment();

    // 16. Let traversable be navigable's traversable navigable.
    auto traversable = traversable_navigable();

    // 17. Append the following session history synchronous navigation steps involving navigable to traversable:
    traversable->append_session_history_synchronous_navigation_steps(*this, GC::create_function(heap(), [this, traversable, history_entry, entry_to_replace, navigation_id, history_handling] {
        // 1. Finalize a same-document navigation given traversable, navigable, historyEntry, and entryToReplace.
        finalize_a_same_document_navigation(*traversable, *this, history_entry, entry_to_replace, history_handling);

        // FIXME: 2. Invoke WebDriver BiDi fragment navigated with navigable's active browsing context and a new WebDriver BiDi
        //            navigation status whose id is navigationId, url is url, and status is "complete".
        (void)navigation_id;
    }));

    return {};
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#evaluate-a-javascript:-url
// https://whatpr.org/html/9893/browsing-the-web.html#evaluate-a-javascript:-url
WebIDL::ExceptionOr<GC::Ptr<DOM::Document>> Navigable::evaluate_javascript_url(URL::URL const& url, URL::Origin const& new_document_origin, String navigation_id)
{
    auto& vm = this->vm();
    VERIFY(active_window());
    auto& realm = active_window()->realm();

    // 1. Let urlString be the result of running the URL serializer on url.
    auto url_string = url.serialize();

    // 2. Let encodedScriptSource be the result of removing the leading "javascript:" from urlString.
    auto encoded_script_source = url_string.substring_view(11, url_string.length() - 11);

    // 3. Let scriptSource be the UTF-8 decoding of the percent-decoding of encodedScriptSource.
    auto script_source = URL::percent_decode(encoded_script_source);

    // 4. Let settings be targetNavigable's active document's relevant settings object.
    auto& settings = active_document()->relevant_settings_object();

    // 5. Let baseURL be settings's API base URL.
    auto base_url = settings.api_base_url();

    // 6. Let script be the result of creating a classic script given scriptSource, settings's realm, baseURL, and the default classic script fetch options.
    auto script = HTML::ClassicScript::create("(javascript url)", script_source, settings.realm(), base_url);

    // 7. Let evaluationStatus be the result of running the classic script script.
    auto evaluation_status = script->run();

    // 8. Let result be null.
    String result;

    // 9. If evaluationStatus is a normal completion, and evaluationStatus.[[Value]] is a String, then set result to evaluationStatus.[[Value]].
    if (evaluation_status.type() == JS::Completion::Type::Normal && evaluation_status.value().has_value() && evaluation_status.value()->is_string()) {
        result = evaluation_status.value()->as_string().utf8_string();
    } else {
        // 10. Otherwise, return null.
        return nullptr;
    }

    // 11. Let response be a new response with
    //     URL: targetNavigable's active document's URL
    //     header list: «(`Content-Type`, `text/html;charset=utf-8`)»
    //     body: the UTF-8 encoding of result, as a body
    auto response = Fetch::Infrastructure::Response::create(vm);
    response->url_list().append(active_document()->url());

    auto header = Fetch::Infrastructure::Header::from_string_pair("Content-Type"sv, "text/html"sv);
    response->header_list()->append(move(header));

    response->set_body(TRY(Fetch::Infrastructure::byte_sequence_as_body(realm, result.bytes())));

    // 12. Let policyContainer be targetNavigable's active document's policy container.
    auto const& policy_container = active_document()->policy_container();

    // FIXME: 13. Let finalSandboxFlags be policyContainer's CSP list's CSP-derived sandboxing flags.
    auto final_sandbox_flags = SandboxingFlagSet {};

    // 14. Let coop be targetNavigable's active document's opener policy.
    auto const& coop = active_document()->opener_policy();

    // 15. Let coopEnforcementResult be a new opener policy enforcement result with
    //     url: url
    //     origin: newDocumentOrigin
    //     opener policy: coop
    OpenerPolicyEnforcementResult coop_enforcement_result {
        .url = url,
        .origin = new_document_origin,
        .opener_policy = coop,
    };

    // 16. Let navigationParams be a new navigation params, with
    //     id: navigationId
    //     navigable: targetNavigable
    //     request: null
    //     response: response
    //     fetch controller: null
    //     commit early hints: null
    //     COOP enforcement result: coopEnforcementResult
    //     reserved environment: null
    //     origin: newDocumentOrigin
    //     policy container: policyContainer
    //     final sandboxing flag set: finalSandboxFlags
    //     opener policy: coop
    // FIXME: navigation timing type: "navigate"
    //     about base URL: targetNavigable's active document's about base URL
    auto navigation_params = vm.heap().allocate<NavigationParams>();
    navigation_params->id = navigation_id;
    navigation_params->navigable = this;
    navigation_params->request = {};
    navigation_params->response = response;
    navigation_params->fetch_controller = nullptr;
    navigation_params->commit_early_hints = nullptr;
    navigation_params->coop_enforcement_result = move(coop_enforcement_result);
    navigation_params->reserved_environment = {};
    navigation_params->origin = new_document_origin;
    navigation_params->policy_container = policy_container;
    navigation_params->final_sandboxing_flag_set = final_sandbox_flags;
    navigation_params->opener_policy = coop;
    navigation_params->about_base_url = active_document()->about_base_url();

    // 17. Return the result of loading an HTML document given navigationParams.
    return load_document(navigation_params);
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#navigate-to-a-javascript:-url
WebIDL::ExceptionOr<void> Navigable::navigate_to_a_javascript_url(URL::URL const& url, HistoryHandlingBehavior history_handling, URL::Origin const& initiator_origin, CSPNavigationType csp_navigation_type, String navigation_id)
{
    // 1. Assert: historyHandling is "replace".
    VERIFY(history_handling == HistoryHandlingBehavior::Replace);

    // 2. Set the ongoing navigation for targetNavigable to null.
    set_ongoing_navigation({});

    // 3. If initiatorOrigin is not same origin-domain with targetNavigable's active document's origin, then return.
    if (!initiator_origin.is_same_origin_domain(active_document()->origin()))
        return {};

    // FIXME: 4. Let request be a new request whose URL is url.

    // FIXME: 5. If the result of should navigation request of type be blocked by Content Security Policy? given request and cspNavigationType is "Blocked", then return.
    (void)csp_navigation_type;

    // 6. Let newDocument be the result of evaluating a javascript: URL given targetNavigable, url, and initiatorOrigin.
    auto new_document = TRY(evaluate_javascript_url(url, initiator_origin, navigation_id));

    // 7. If newDocument is null, then return.
    if (!new_document) {
        // NOTE: In this case, some JavaScript code was executed, but no new Document was created, so we will not perform a navigation.
        return {};
    }

    // 8. Assert: initiatorOrigin is newDocument's origin.
    VERIFY(initiator_origin == new_document->origin());

    // 9. Let entryToReplace be targetNavigable's active session history entry.
    auto entry_to_replace = active_session_history_entry();

    // 10. Let oldDocState be entryToReplace's document state.
    auto old_doc_state = entry_to_replace->document_state();

    // 11. Let documentState be a new document state with
    //     document: newDocument
    //     history policy container: a clone of the oldDocState's history policy container if it is non-null; null otherwise
    //     request referrer: oldDocState's request referrer
    //     request referrer policy: oldDocState's request referrer policy
    //     initiator origin: initiatorOrigin
    //     origin: initiatorOrigin
    //     about base URL: oldDocState's about base URL
    //     resource: null
    //     ever populated: true
    //     navigable target name: oldDocState's navigable target name
    GC::Ref<DocumentState> document_state = *heap().allocate<DocumentState>();
    document_state->set_document(new_document);
    document_state->set_history_policy_container(old_doc_state->history_policy_container());
    document_state->set_request_referrer(old_doc_state->request_referrer());
    document_state->set_request_referrer_policy(old_doc_state->request_referrer_policy());
    document_state->set_initiator_origin(initiator_origin);
    document_state->set_origin(initiator_origin);
    document_state->set_about_base_url(old_doc_state->about_base_url());
    document_state->set_ever_populated(true);
    document_state->set_navigable_target_name(old_doc_state->navigable_target_name());

    // 12. Let historyEntry be a new session history entry, with
    //     URL: entryToReplace's URL
    //     document state: documentState
    GC::Ref<SessionHistoryEntry> history_entry = *heap().allocate<SessionHistoryEntry>();
    history_entry->set_url(entry_to_replace->url());
    history_entry->set_document_state(document_state);

    // 13. Append session history traversal steps to targetNavigable's traversable to finalize a cross-document navigation with targetNavigable, historyHandling, and historyEntry.
    traversable_navigable()->append_session_history_traversal_steps(GC::create_function(heap(), [this, history_entry, history_handling, navigation_id] {
        finalize_a_cross_document_navigation(*this, history_handling, history_entry);
    }));

    return {};
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#reload
void Navigable::reload()
{
    // 1. Set navigable's active session history entry's document state's reload pending to true.
    active_session_history_entry()->document_state()->set_reload_pending(true);

    // 2. Let traversable be navigable's traversable navigable.
    auto traversable = traversable_navigable();

    // 3. Append the following session history traversal steps to traversable:
    traversable->append_session_history_traversal_steps(GC::create_function(heap(), [traversable] {
        // 1. Apply the reload history step to traversable.
        traversable->apply_the_reload_history_step();
    }));
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#the-navigation-must-be-a-replace
bool navigation_must_be_a_replace(URL::URL const& url, DOM::Document const& document)
{
    return url.scheme() == "javascript"sv || document.is_initial_about_blank();
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#allowed-to-navigate
bool Navigable::allowed_by_sandboxing_to_navigate(Navigable const& target, SourceSnapshotParams const& source_snapshot_params)
{
    auto& source = *this;

    auto is_ancestor_of = [](Navigable const& a, Navigable const& b) {
        for (auto parent = b.parent(); parent; parent = parent->parent()) {
            if (parent.ptr() == &a)
                return true;
        }
        return false;
    };

    // A navigable source is allowed by sandboxing to navigate a second navigable target,
    // given a source snapshot params sourceSnapshotParams, if the following steps return true:

    // 1. If source is target, then return true.
    if (&source == &target)
        return true;

    // 2. If source is an ancestor of target, then return true.
    if (is_ancestor_of(source, target))
        return true;

    // 3. If target is an ancestor of source, then:
    if (is_ancestor_of(target, source)) {

        // 1. If target is not a top-level traversable, then return true.
        if (!target.is_top_level_traversable())
            return true;

        // 2. If sourceSnapshotParams's has transient activation is true, and sourceSnapshotParams's sandboxing flags's
        //    sandboxed top-level navigation with user activation browsing context flag is set, then return false.
        if (source_snapshot_params.has_transient_activation && has_flag(source_snapshot_params.sandboxing_flags, SandboxingFlagSet::SandboxedTopLevelNavigationWithUserActivation))
            return false;

        // 3. If sourceSnapshotParams's has transient activation is false, and sourceSnapshotParams's sandboxing flags's
        //    sandboxed top-level navigation without user activation browsing context flag is set, then return false.
        if (!source_snapshot_params.has_transient_activation && has_flag(source_snapshot_params.sandboxing_flags, SandboxingFlagSet::SandboxedTopLevelNavigationWithoutUserActivation))
            return false;

        // 4. Return true.
        return true;
    }

    // 4. If target is a top-level traversable:
    if (target.is_top_level_traversable()) {
        // FIXME: 1. If source is the one permitted sandboxed navigator of target, then return true.

        // 2. If sourceSnapshotParams's sandboxing flags's sandboxed navigation browsing context flag is set, then return false.
        if (has_flag(source_snapshot_params.sandboxing_flags, SandboxingFlagSet::SandboxedNavigation))
            return false;

        // 3. Return true.
        return true;
    }

    // 5. If sourceSnapshotParams's sandboxing flags's sandboxed navigation browsing context flag is set, then return false.
    // 6. Return true.
    return !has_flag(source_snapshot_params.sandboxing_flags, SandboxingFlagSet::SandboxedNavigation);
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#snapshotting-target-snapshot-params
TargetSnapshotParams Navigable::snapshot_target_snapshot_params()
{
    // To snapshot target snapshot params given a navigable targetNavigable, return a new target snapshot params
    // with sandboxing flags set to the result of determining the creation sandboxing flags given targetNavigable's
    // active browsing context and targetNavigable's container.

    return { determine_the_creation_sandboxing_flags(*active_browsing_context(), container()) };
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#finalize-a-cross-document-navigation
void finalize_a_cross_document_navigation(GC::Ref<Navigable> navigable, HistoryHandlingBehavior history_handling, GC::Ref<SessionHistoryEntry> history_entry)
{
    // NOTE: This is not in the spec but we should not navigate destroyed navigable.
    if (navigable->has_been_destroyed())
        return;

    // 1. FIXME: Assert: this is running on navigable's traversable navigable's session history traversal queue.

    // 2. Set navigable's is delaying load events to false.
    navigable->set_delaying_load_events(false);

    // 3. If historyEntry's document is null, then return.
    if (!history_entry->document())
        return;

    // 4. If all of the following are true:
    //    - navigable's parent is null;
    //    - historyEntry's document's browsing context is not an auxiliary browsing context whose opener browsing context is non-null; and
    //    - historyEntry's document's origin is not navigable's active document's origin
    //    then set historyEntry's document state's navigable target name to the empty string.
    if (navigable->parent() == nullptr
        && !(history_entry->document()->browsing_context()->is_auxiliary() && history_entry->document()->browsing_context()->opener_browsing_context() != nullptr)
        && history_entry->document()->origin() != navigable->active_document()->origin()) {
        history_entry->document_state()->set_navigable_target_name(String {});
    }

    // 5. Let entryToReplace be navigable's active session history entry if historyHandling is "replace", otherwise null.
    auto entry_to_replace = history_handling == HistoryHandlingBehavior::Replace ? navigable->active_session_history_entry() : nullptr;

    // 6. Let traversable be navigable's traversable navigable.
    auto traversable = navigable->traversable_navigable();

    // 7. Let targetStep be null.
    int target_step;

    // 8. Let targetEntries be the result of getting session history entries for navigable.
    auto& target_entries = navigable->get_session_history_entries();

    // 9. If entryToReplace is null, then:
    if (entry_to_replace == nullptr) {
        // 1. Clear the forward session history of traversable.
        traversable->clear_the_forward_session_history();

        // 2. Set targetStep to traversable's current session history step + 1.
        target_step = traversable->current_session_history_step() + 1;

        // 3. Set historyEntry's step to targetStep.
        history_entry->set_step(target_step);

        // 4. Append historyEntry to targetEntries.
        target_entries.append(history_entry);
    } else {
        // 1. Replace entryToReplace with historyEntry in targetEntries.
        *(target_entries.find(*entry_to_replace)) = history_entry;

        // 2. Set historyEntry's step to entryToReplace's step.
        history_entry->set_step(entry_to_replace->step());

        // 3. If historyEntry's document state's origin is same origin with entryToReplace's document state's origin,
        //    then set historyEntry's navigation API key to entryToReplace's navigation API key.
        if (history_entry->document_state()->origin().has_value() && entry_to_replace->document_state()->origin().has_value() && history_entry->document_state()->origin()->is_same_origin(*entry_to_replace->document_state()->origin())) {
            history_entry->set_navigation_api_key(entry_to_replace->navigation_api_key());
        }

        // 4. Set targetStep to traversable's current session history step.
        target_step = traversable->current_session_history_step();
    }

    // 10. Apply the push/replace history step targetStep to traversable.
    traversable->apply_the_push_or_replace_history_step(target_step, history_handling, TraversableNavigable::SynchronousNavigation::No);
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#url-and-history-update-steps
void perform_url_and_history_update_steps(DOM::Document& document, URL::URL new_url, Optional<SerializationRecord> serialized_data, HistoryHandlingBehavior history_handling)
{
    // 1. Let navigable be document's node navigable.
    auto navigable = document.navigable();

    // 2. Let activeEntry be navigable's active session history entry.
    auto active_entry = navigable->active_session_history_entry();

    // 3. Let newEntry be a new session history entry, with
    //      URL: newURL
    //      serialized state: if serializedData is not null, serializedData; otherwise activeEntry's classic history API state
    //      document state: activeEntry's document state
    //      scroll restoration mode: activeEntry's scroll restoration mode
    // FIXME: persisted user state: activeEntry's persisted user state
    GC::Ref<SessionHistoryEntry> new_entry = document.heap().allocate<SessionHistoryEntry>();
    new_entry->set_url(new_url);
    new_entry->set_classic_history_api_state(serialized_data.value_or(active_entry->classic_history_api_state()));
    new_entry->set_document_state(active_entry->document_state());
    new_entry->set_scroll_restoration_mode(active_entry->scroll_restoration_mode());

    // 4. If document's is initial about:blank is true, then set historyHandling to "replace".
    if (document.is_initial_about_blank()) {
        history_handling = HistoryHandlingBehavior::Replace;
    }

    // 5. Let entryToReplace be activeEntry if historyHandling is "replace", otherwise null.
    auto entry_to_replace = history_handling == HistoryHandlingBehavior::Replace ? active_entry : nullptr;

    // 6. If historyHandling is "push", then:
    if (history_handling == HistoryHandlingBehavior::Push) {
        // 1. Increment document's history object's index.
        document.history()->m_index++;

        // 2. Set document's history object's length to its index + 1.
        document.history()->m_length = document.history()->m_index + 1;
    }

    // If serializedData is not null, then restore the history object state given document and newEntry.
    if (serialized_data.has_value())
        document.restore_the_history_object_state(new_entry);

    // 8. Set document's URL to newURL.
    document.set_url(new_url);

    // 9. Set document's latest entry to newEntry.
    document.set_latest_entry(new_entry);

    // 10. Set navigable's active session history entry to newEntry.
    navigable->set_active_session_history_entry(new_entry);

    // 11. Update the navigation API entries for a same-document navigation given document's relevant global object's navigation API, newEntry, and historyHandling.
    auto& relevant_global_object = verify_cast<Window>(HTML::relevant_global_object(document));
    auto navigation_type = history_handling == HistoryHandlingBehavior::Push ? Bindings::NavigationType::Push : Bindings::NavigationType::Replace;
    relevant_global_object.navigation()->update_the_navigation_api_entries_for_a_same_document_navigation(new_entry, navigation_type);

    // 12. Let traversable be navigable's traversable navigable.
    auto traversable = navigable->traversable_navigable();

    // 13. Append the following session history synchronous navigation steps involving navigable to traversable:
    traversable->append_session_history_synchronous_navigation_steps(*navigable, GC::create_function(document.realm().heap(), [traversable, navigable, new_entry, entry_to_replace, history_handling] {
        // 1. Finalize a same-document navigation given traversable, navigable, newEntry, and entryToReplace.
        finalize_a_same_document_navigation(*traversable, *navigable, new_entry, entry_to_replace, history_handling);

        // 2. FIXME: Invoke WebDriver BiDi history updated with navigable.
    }));
}

void Navigable::scroll_offset_did_change()
{
    // https://w3c.github.io/csswg-drafts/cssom-view-1/#scrolling-events
    // Whenever a viewport gets scrolled (whether in response to user interaction or by an API), the user agent must run these steps:

    // 1. Let doc be the viewport’s associated Document.
    auto doc = active_document();
    VERIFY(doc);

    // 2. If doc is already in doc’s pending scroll event targets, abort these steps.
    for (auto& target : doc->pending_scroll_event_targets()) {
        if (target.ptr() == doc)
            return;
    }

    // 3. Append doc to doc’s pending scroll event targets.
    doc->pending_scroll_event_targets().append(*doc);
}

CSSPixelRect Navigable::to_top_level_rect(CSSPixelRect const& a_rect)
{
    auto rect = a_rect;
    rect.set_location(to_top_level_position(a_rect.location()));
    return rect;
}

CSSPixelPoint Navigable::to_top_level_position(CSSPixelPoint a_position)
{
    auto position = a_position;
    for (auto ancestor = parent(); ancestor; ancestor = ancestor->parent()) {
        if (is<TraversableNavigable>(*ancestor))
            break;
        if (!ancestor->container())
            return {};
        if (!ancestor->container()->paintable())
            return {};
        position.translate_by(ancestor->container()->paintable()->box_type_agnostic_position());
    }
    return position;
}

void Navigable::set_viewport_size(CSSPixelSize size)
{
    if (m_size == size)
        return;

    m_size = size;
    if (auto document = active_document()) {
        // NOTE: Resizing the viewport changes the reference value for viewport-relative CSS lengths.
        document->invalidate_style(DOM::StyleInvalidationReason::NavigableSetViewportSize);
        document->set_needs_layout();
    }

    if (auto document = active_document()) {
        document->set_needs_display(InvalidateDisplayList::No);

        document->inform_all_viewport_clients_about_the_current_viewport_rect();

        // Schedule the HTML event loop to ensure that a `resize` event gets fired.
        HTML::main_thread_event_loop().schedule();
    }
}

void Navigable::perform_scroll_of_viewport(CSSPixelPoint new_position)
{
    if (m_viewport_scroll_offset != new_position) {
        m_viewport_scroll_offset = new_position;
        scroll_offset_did_change();

        if (auto document = active_document()) {
            document->set_needs_display(InvalidateDisplayList::No);
            document->set_needs_to_refresh_scroll_state(true);
            document->inform_all_viewport_clients_about_the_current_viewport_rect();
        }
    }

    // Schedule the HTML event loop to ensure that a `resize` event gets fired.
    HTML::main_thread_event_loop().schedule();
}

void Navigable::set_needs_display(InvalidateDisplayList should_invalidate_display_list)
{
    if (auto document = active_document(); document) {
        document->set_needs_display(should_invalidate_display_list);
    }
}

// https://html.spec.whatwg.org/#rendering-opportunity
bool Navigable::has_a_rendering_opportunity() const
{
    // A navigable has a rendering opportunity if the user agent is currently able to present
    // the contents of the navigable to the user,
    // accounting for hardware refresh rate constraints and user agent throttling for performance reasons,
    // but considering content presentable even if it's outside the viewport.

    // A navigable has no rendering opportunities if its active document is render-blocked
    // or if it is suppressed for view transitions;
    // otherwise, rendering opportunities are determined based on hardware constraints
    // such as display refresh rates and other factors such as page performance
    // or whether the document's visibility state is "visible".
    // Rendering opportunities typically occur at regular intervals.

    // FIXME: Return `false` here if we're an inactive browser tab.
    auto browsing_context = const_cast<Navigable*>(this)->active_browsing_context();
    if (!browsing_context)
        return false;
    return browsing_context->page().client().is_ready_to_paint();
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#inform-the-navigation-api-about-aborting-navigation
void Navigable::inform_the_navigation_api_about_aborting_navigation()
{
    // FIXME: 1. If this algorithm is running on navigable's active window's relevant agent's event loop, then continue on to the following steps.
    // Otherwise, queue a global task on the navigation and traversal task source given navigable's active window to run the following steps.

    // AD-HOC: Not in the spec but subsequent steps will fail if the navigable doesn't have an active window.
    if (!active_window())
        return;

    queue_global_task(Task::Source::NavigationAndTraversal, *active_window(), GC::create_function(heap(), [this] {
        // 2. Let navigation be navigable's active window's navigation API.
        VERIFY(active_window());
        auto navigation = active_window()->navigation();

        // 3. If navigation's ongoing navigate event is null, then return.
        if (navigation->ongoing_navigate_event() == nullptr)
            return;

        // 4. Abort the ongoing navigation given navigation.
        navigation->abort_the_ongoing_navigation();
    }));
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#event-uni
UserNavigationInvolvement user_navigation_involvement(DOM::Event const& event)
{
    // For convenience at certain call sites, the user navigation involvement for an Event event is defined as follows:

    // 1. Assert: this algorithm is being called as part of an activation behavior definition.
    // 2. Assert: event's type is "click".
    VERIFY(event.type() == "click"_fly_string);

    // 3. If event's isTrusted is initialized to true, then return "activation".
    // 4. Return "none".
    return event.is_trusted() ? UserNavigationInvolvement::Activation : UserNavigationInvolvement::None;
}

bool Navigable::is_focused() const
{
    return &m_page->focused_navigable() == this;
}

static String visible_text_in_range(DOM::Range const& range)
{
    // NOTE: This is an adaption of Range stringification, but we skip over DOM nodes that don't have a corresponding layout node.
    StringBuilder builder;

    if (range.start_container() == range.end_container() && is<DOM::Text>(*range.start_container())) {
        if (!range.start_container()->layout_node())
            return String {};
        return MUST(static_cast<DOM::Text const&>(*range.start_container()).data().substring_from_byte_offset(range.start_offset(), range.end_offset() - range.start_offset()));
    }

    if (is<DOM::Text>(*range.start_container()) && range.start_container()->layout_node())
        builder.append(static_cast<DOM::Text const&>(*range.start_container()).data().bytes_as_string_view().substring_view(range.start_offset()));

    for (DOM::Node const* node = range.start_container(); node != range.end_container()->next_sibling(); node = node->next_in_pre_order()) {
        if (is<DOM::Text>(*node) && range.contains_node(*node) && node->layout_node())
            builder.append(static_cast<DOM::Text const&>(*node).data());
    }

    if (is<DOM::Text>(*range.end_container()) && range.end_container()->layout_node())
        builder.append(static_cast<DOM::Text const&>(*range.end_container()).data().bytes_as_string_view().substring_view(0, range.end_offset()));

    return MUST(builder.to_string());
}

String Navigable::selected_text() const
{
    auto document = const_cast<Navigable*>(this)->active_document();
    if (!document)
        return String {};
    auto selection = const_cast<DOM::Document&>(*document).get_selection();
    auto range = selection->range();
    if (!range)
        return String {};
    return visible_text_in_range(*range);
}

void Navigable::select_all()
{
    auto document = active_document();
    if (!document)
        return;

    auto selection = document->get_selection();
    if (!selection)
        return;

    if (auto target = document->active_input_events_target()) {
        target->select_all();
    } else if (auto* body = document->body()) {
        (void)selection->select_all_children(*body);
    }
}

void Navigable::paste(String const& text)
{
    auto document = active_document();
    if (!document)
        return;

    m_event_handler.handle_paste(text);
}

void Navigable::register_navigation_observer(Badge<NavigationObserver>, NavigationObserver& navigation_observer)
{
    auto result = m_navigation_observers.set(navigation_observer);
    VERIFY(result == AK::HashSetResult::InsertedNewEntry);
}

void Navigable::unregister_navigation_observer(Badge<NavigationObserver>, NavigationObserver& navigation_observer)
{
    bool was_removed = m_navigation_observers.remove(navigation_observer);
    VERIFY(was_removed);
}

}
