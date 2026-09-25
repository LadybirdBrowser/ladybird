/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibGC/Heap.h>
#include <LibURL/Origin.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/BrowsingContextGroup.h>
#include <LibWeb/HTML/DocumentState.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/HTMLIFrameElement.h>
#include <LibWeb/HTML/HistoryExecutor.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/LocalTraversableNavigable.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/HTML/NavigationParams.h>
#include <LibWeb/HTML/RemoteNavigable.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/WindowEnvironmentSettingsObject.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/BoxViews.h>

namespace Web::HTML {

HashTable<NavigableContainer*>& NavigableContainer::all_instances()
{
    static NeverDestroyed<HashTable<NavigableContainer*>> set;
    return *set;
}

NavigableContainer::NavigableContainer(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
    all_instances().set(this);
}

NavigableContainer::~NavigableContainer() = default;

void NavigableContainer::finalize()
{
    Base::finalize();
    all_instances().remove(this);
}

void NavigableContainer::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_content_navigable);
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#create-a-new-child-navigable
void NavigableContainer::create_new_child_navigable()
{
    // 1. Let parentNavigable be element's node navigable.
    auto parent_navigable = navigable();

    // 2. Let group be element's node document's browsing context's top-level browsing context's group.
    // NB: The UI process places documents in agents from its canonical browsing context group, so group is not
    //     resolved here.

    // 3. Let browsingContext and document be the result of creating a new browsing context and document given element's node document, element, and group.
    auto& page = document().page();
    auto [browsing_context, document] = BrowsingContext::create_a_new_browsing_context_and_document(page, this->document(), *this);

    // 4. Let targetName be null.
    Optional<Utf16String> target_name;

    // 5. If element has a name content attribute, then set targetName to the value of that attribute.
    if (name().has_value())
        target_name = name().value().to_utf16_string();

    // 6. Let documentState be a new document state, with
    //  - document: document
    //  - initiator origin: document's origin
    //  - origin: document's origin
    //  - navigable target name: targetName
    //  - about base URL: document's about base URL
    auto document_state = HTML::DocumentState::create(document->page().client().allocate_cross_process_id());
    document_state->set_initiator_origin(document->origin());
    document_state->set_origin(document->origin());
    if (target_name.has_value())
        document_state->set_navigable_target_name(*target_name);
    document_state->set_about_base_url(document->about_base_url());

    // 7. Let navigable be a new navigable.
    GC::Ref<LocalNavigable> navigable = *GC::Heap::the().allocate<LocalNavigable>(page, false);

    // 8. Initialize the navigable navigable given documentState and parentNavigable.
    navigable->initialize_navigable(document_state, parent_navigable, *document, parent_navigable->active_document()->visibility_state());
    navigable->inherit_page_state_from(*parent_navigable);

    // 9. Set element's content navigable to navigable.
    m_content_navigable = navigable;
    m_reported_content_navigable_viewport = {};
    navigable->set_container({}, this);

    if (auto* layout_node = unsafe_layout_node())
        layout_node->refresh_dom_paint_facts();
    set_needs_repaint();

    (void)parent_navigable->adopt_canonical_id_for_child_created_during_history_reconstruction(navigable);

    // 10. Let historyEntry be navigable's active session history entry.
    auto history_entry = navigable->active_session_history_entry();

    page.client().page_did_create_child_frame(parent_navigable->id(), navigable->id(), navigable->replicated_state(), create_pending_session_history_entry_descriptor(*history_entry));

    // 12. Append the following session history traversal steps to traversable:
    page.history_executor().request_history_operation(
        NavigableCreationHistoryOperationParameters {
            .parent_navigable_id = parent_navigable->id(),
            .navigable_id = navigable->id(),
        },
        {
            .local_target_navigable_id = navigable->id(),
            .local_target_entry = history_entry,
            .pre_steps = GC::create_function(heap(), [navigable, parent_navigable, history_entry](Optional<Web::ReconstructedChildNavigation> reconstructed_child_navigation, GC::Ref<HistoryExecutor::OnHistoryOperationReady> ready) mutable {
                if (navigable->has_been_destroyed() || parent_navigable->has_been_destroyed()) {
                    ready->function()(HistoryStepResult::Applied);
                    return;
                }

                // 1-6. Append nestedHistory to parentDocState's nested histories.
                if (reconstructed_child_navigation.has_value()) {
                    navigable->route_child_created_during_history_reconstruction(reconstructed_child_navigation.release_value());
                    ready->function()(HistoryStepResult::Applied);
                    return;
                }

                auto parent_document_state = parent_navigable->active_session_history_entry()->document_state();

                // 7. Update for navigable creation/destruction given traversable
                ready->function()(parent_document_state->cross_process_id());
            }),
            .on_complete = GC::create_function(heap(), [this, navigable](HistoryStepResult) {
                if (navigable->has_been_destroyed() || content_navigable() != navigable)
                    return;
                navigable->set_has_session_history_entry_and_ready_for_navigation();
                this->document().schedule_html_parser_end_check();
            }),
        });
}

// https://html.spec.whatwg.org/multipage/browsers.html#concept-bcc-content-document
DOM::Document const* NavigableContainer::content_document() const
{
    // 1. If container's content navigable is null, then return null.
    if (m_content_navigable == nullptr)
        return nullptr;

    // 2. Let document be container's content navigable's active document.
    // NB: A document hosted by another process is never same origin-domain with container's node document here, until
    //     same-origin documents of different pages are stitched together.
    auto* local_navigable = as_if<LocalNavigable>(*m_content_navigable);
    if (!local_navigable)
        return nullptr;
    auto document = local_navigable->active_document();

    // AD-HOC: The active document can be null during navigation, after the old document
    //         has been destroyed but before the new document has been set.
    if (!document)
        return nullptr;

    // 3. If document's origin and container's node document's origin are not same origin-domain, then return null.
    if (!document->origin().is_same_origin_domain(m_document->origin()))
        return nullptr;

    // 4. Return document.
    return document.ptr();
}

DOM::Document const* NavigableContainer::content_document_without_origin_check() const
{
    if (!m_content_navigable)
        return nullptr;

    // A document hosted by another process is not here.
    auto* local_navigable = as_if<LocalNavigable>(*m_content_navigable);
    if (!local_navigable)
        return nullptr;
    return local_navigable->active_document().ptr();
}

// https://html.spec.whatwg.org/multipage/embedded-content-other.html#dom-media-getsvgdocument
DOM::Document const* NavigableContainer::get_svg_document() const
{
    // 1. Let document be this element's content document.
    auto const* document = content_document();

    // 2. If document is non-null and was created by the page load processing model for XML files section because the computed type of the resource in the navigate algorithm was image/svg+xml, then return document.
    if (document && document->content_type() == u"image/svg+xml"sv)
        return document;
    // 3. Return null.
    return nullptr;
}

HTML::WindowProxy* NavigableContainer::content_window()
{
    if (!m_content_navigable)
        return nullptr;
    return m_content_navigable->active_window_proxy().ptr();
}

// https://html.spec.whatwg.org/multipage/iframe-embed-object.html#shared-attribute-processing-steps-for-iframe-and-frame-elements
Optional<URL::URL> NavigableContainer::shared_attribute_processing_steps_for_iframe_and_frame(InitialInsertion initial_insertion)
{
    if (!navigable())
        return OptionalNone {};

    // AD-HOC: If the element was added and immediately removed, the content navigable will be null. Don't process the
    //         src attribute any further.
    if (!m_content_navigable)
        return {};

    // 1. Let url be the URL record about:blank.
    auto url = URL::about_blank();

    // 2. If element has a src attribute specified, and its value is not the empty string, then:
    auto src_attribute_value = attribute(HTML::AttributeNames::src);
    if (src_attribute_value.has_value() && !src_attribute_value->is_empty()) {
        // 1. Let maybeURL be the result of encoding-parsing a URL given that attribute's value, relative to element's node document.
        auto maybe_url = document().encoding_parse_url(*src_attribute_value);

        // 2. If maybeURL is not failure, then set url to maybeURL.
        if (maybe_url.has_value())
            url = maybe_url.release_value();
    }

    // 3. If the inclusive ancestor navigables of element's node navigable contains a navigable
    //    whose active document's URL equals url with exclude fragments set to true, then return null.
    for (auto const& navigable : document().inclusive_ancestor_navigables()) {
        if (navigable->active_document_url()->equals(url, URL::ExcludeFragment::Yes))
            return {};
    }

    // 4. If url matches about:blank and initialInsertion is true, then perform the URL and history update steps given element's content navigable's active document and url.
    if (url_matches_about_blank(url) && initial_insertion == InitialInsertion::Yes) {
        // NB: A newly inserted element's content navigable was created in this process.
        auto& local_navigable = as<LocalNavigable>(*m_content_navigable);

        // AD-HOC: If the content navigable already has a navigation in progress or pending, skip the initial
        //         about:blank URL update. Without this, the URL update creates a state machine that clobbers the
        //         navigable's ongoing_navigation, causing the real navigation to be dropped when its populate completion
        //         callback checks ongoing_navigation != navigation_id. Non-blank src navigations must still be processed
        //         here, and will be queued by LocalNavigable::navigate() until the child navigable is ready for navigation.
        if (local_navigable.has_pending_navigations() || !local_navigable.ongoing_navigation().has<Empty>())
            return {};

        perform_url_and_history_update_steps(*local_navigable.active_document(), url);
    }

    // 5. Return url.
    return url;
}

// https://html.spec.whatwg.org/multipage/iframe-embed-object.html#navigate-an-iframe-or-frame
void NavigableContainer::navigate_an_iframe_or_frame(URL::URL url, ReferrerPolicy::ReferrerPolicy referrer_policy, Optional<Utf16String> srcdoc_string, InitialInsertion initial_insertion)
{
    // 1. Let historyHandling be "auto".
    auto history_handling = NavigationHistoryBehavior::Auto;

    // 2. If element's content navigable's active document is not completely loaded, then set historyHandling to "replace".
    // AD-HOC: Only apply this check during initial insertion. For subsequent attribute-driven navigations,
    //         the previous document may have parsed and run scripts but not yet fired its load event;
    //         forcing "replace" in that case would incorrectly discard the history entry.
    if (initial_insertion == InitialInsertion::Yes) {
        // NB: A newly inserted element's content navigable was created in this process.
        auto active_document = as<LocalNavigable>(*m_content_navigable).active_document();
        if (active_document && !active_document->is_completely_loaded())
            history_handling = NavigationHistoryBehavior::Replace;
    }

    // 3. If element is an iframe:
    if (auto* iframe = as_if<HTMLIFrameElement>(this)) {
        // 1. Set element's pending resource-timing start time to the current high resolution time given element's node
        //    document's relevant global object.
        iframe->set_pending_resource_start_time(HighResolutionTime::current_high_resolution_time(relevant_global_object(document())));

        // 2. Set element's pending resource-timing URL to url.
        iframe->set_pending_resource_timing_url(url);
    }

    // 4. Navigate element's content navigable to url using element's node document, with historyHandling set to historyHandling,
    //    referrerPolicy set to referrerPolicy, documentResource set to srcdocString, and initialInsertion set to
    //    initialInsertion.
    DocumentResource document_resource = Empty {};
    if (srcdoc_string.has_value())
        document_resource = *srcdoc_string;

    MUST(m_content_navigable->navigate({
        .url = move(url),
        .source_document = document(),
        .document_resource = document_resource,
        .history_handling = history_handling,
        .referrer_policy = referrer_policy,
        .initial_insertion = initial_insertion,
    }));
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#destroy-a-child-navigable
void NavigableContainer::destroy_the_child_navigable()
{
    // 1. Let navigable be container's content navigable.
    auto navigable = content_navigable();

    // 2. If navigable is null, then return.
    if (!navigable)
        return;

    if (navigable->has_been_destroyed())
        return;
    navigable->set_has_been_destroyed();

    // 3. Set container's content navigable to null.
    m_content_navigable = nullptr;
    m_reported_content_navigable_viewport = {};
    navigable->set_container({}, nullptr);
    document().schedule_html_parser_end_check();
    if (auto* layout_node = unsafe_layout_node())
        layout_node->refresh_dom_paint_facts();
    set_needs_repaint();

    // The load-event delays and navigation API of the navigable's document are where the document is.
    if (auto* local_navigable = as_if<LocalNavigable>(*navigable)) {
        // AD-HOC: Clear the navigable's "is delaying load events" flag.
        //         This removes the DocumentLoadEventDelayer on the parent document that was
        //         created when the navigable started loading (navigate algorithm step 15).
        //         Without this, the delayer lingers until GC collects the LocalNavigable, which can
        //         block the parent document's load event indefinitely.
        local_navigable->set_delaying_load_events(false);

        // AD-HOC: Clear the navigation load event guard that may have been set by
        //         finalize_a_cross_document_navigation. Without this, the guard's
        //         DocumentLoadEventDelayer on the parent document persists until GC,
        //         blocking the parent's load event indefinitely.
        local_navigable->clear_navigation_load_event_guard();

        // 4. Inform the navigation API about child navigable destruction given navigable.
        local_navigable->inform_the_navigation_api_about_child_navigable_destruction();

        // AD-HOC: The spec assumes the active document is non-null in step 5, but during an ancestor
        //         unload the child documents are unloaded (and destroyed) before the ancestor's
        //         pagehide fires. If that pagehide handler then removes a subtree containing this
        //         container, we reach step 5 with navigable's active document already null. We
        //         treat the unload step as a no-op in that case and proceed with the remaining
        //         post-destruction cleanup.
        if (!local_navigable->active_document()) {
            finish_destroying_the_child_navigable(*navigable);
            return;
        }
    }
    // NB: A navigable hosted by another process informs its navigation API there, when the UI process's walk reaches
    //     its document.

    // 5. Destroy a document and its descendants given navigable's active document.
    // AD-HOC: We unload the document and its descendants, instead of just destroying. Unloading fires pagehide at the
    //         document's relevant global object, updates its visibility state to "hidden" (firing visibilitychange),
    //         and fires unload — before destroying the document. The spec as written would leave a removed container's
    //         content documents reporting a "visible" visibility state — with no events fired; Gecko/WebKit/Blink all
    //         fire those events, and report such documents as hidden. This also means starting a view transition in a
    //         removed document skips the transition — since startViewTransition() skips transitions for hidden docs.
    //         See https://github.com/whatwg/html/issues/12288
    // NB: The UI process runs the walk over the navigable's subtree, unloading each document in the page hosting it,
    //     and the navigable's own document too when another page hosts it. It then continues the destruction here.
    document().page().hold_navigable_being_destroyed({}, *navigable);
    document().page().client().page_did_request_child_navigable_unload(navigable->id());
}

// https://html.spec.whatwg.org/multipage/document-lifecycle.html#unload-a-document-and-its-descendants
// NB: The UI process continues the unload of destroy_the_child_navigable() here, once it has unloaded the documents of
//     navigable's descendants where they are hosted.
void NavigableContainer::continue_destroying_the_child_navigable(Navigable& navigable)
{
    // 6. Queue a global task on the navigation and traversal task source given document's relevant global object to
    //    perform the following steps:
    queue_a_task(Task::Source::NavigationAndTraversal, nullptr, nullptr, GC::create_function(navigable.heap(), [navigable = GC::Ref { navigable }] {
        // 1. If firePageSwapSteps is given, then run firePageSwapSteps.
        // 2. Unload document, passing along newDocument if it is not null.
        // NB: The document of a navigable hosted by another process was unloaded there by the UI process.
        if (auto* local_navigable = as_if<LocalNavigable>(*navigable)) {
            if (auto active_document = local_navigable->active_document())
                active_document->unload();
        }

        // 3. If afterAllUnloads was given, then run it.
        finish_destroying_the_child_navigable(*navigable);
    }));
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#destroy-a-child-navigable
void NavigableContainer::finish_destroying_the_child_navigable(Navigable& navigable)
{
    navigable.page().release_navigable_being_destroyed({}, navigable);

    // Not in the spec:
    navigable.page().client().page_did_destroy_child_frame(navigable.id());
    if (auto* local_navigable = as_if<LocalNavigable>(navigable))
        local_navigable->remove_from_all_local_navigables();
    else
        as<RemoteNavigable>(navigable).remove_from_all_remote_navigables();

    // 6. Let parentDocState be container's node navigable's active session history entry's document state.
    // NB: The container may have been inserted into another document by the time the unload finishes, and navigable's
    //     parent is the container's node navigable from when navigable was destroyed.
    auto& parent_navigable = as<LocalNavigable>(*navigable.parent());
    // AD-HOC: The container's node navigable can have been destroyed while the UI process unloaded navigable's subtree.
    if (parent_navigable.has_been_destroyed())
        return;
    auto parent_doc_state = parent_navigable.active_session_history_entry()->document_state();

    // 7. Remove the nested history from parentDocState's nested histories whose id equals navigable's id.
    // NB: The UI process performs this step in canonical session history.

    // 8. Let traversable be container's node navigable's traversable navigable.
    // 9. Append the following session history traversal steps to traversable:
    // 1. Update for navigable creation/destruction given traversable.
    parent_navigable.page().history_executor().request_history_operation(NavigableDestructionHistoryOperationParameters {
        .parent_navigable_id = parent_navigable.id(),
        .parent_document_state_id = parent_doc_state->cross_process_id(),
        .navigable_id = navigable.id(),
    });
}

// AD-HOC: The UI process chose another process to host the content navigable's next document. A RemoteNavigable
//         represents the navigable here from then on, with the WindowProxy scripts hold for it.
void NavigableContainer::swap_content_navigable_to_remote(Badge<Page>, ReplicatedNavigableState replicated_state)
{
    auto& local_navigable = as<LocalNavigable>(*m_content_navigable);
    auto remote_navigable = RemoteNavigable::create(document().page(), local_navigable.id(), local_navigable.parent(), move(replicated_state));
    remote_navigable->set_container({}, this);
    // The document the navigable displayed here was unloaded, after its descendants in the pages hosting them. The
    // WindowProxy scripts hold stays theirs.
    VERIFY(!local_navigable.active_document());
    if (auto window_proxy = local_navigable.window_proxy_after_unload()) {
        remote_navigable->set_window_proxy(*window_proxy);
        window_proxy->set_window(remote_navigable->active_window());
    }
    m_content_navigable = remote_navigable;
    if (auto* layout_node = unsafe_layout_node())
        layout_node->refresh_dom_paint_facts();
    set_needs_repaint();

    local_navigable.set_container({}, nullptr);
    local_navigable.set_delaying_load_events(false);
    local_navigable.clear_navigation_load_event_guard();
    local_navigable.set_has_been_destroyed();
    local_navigable.remove_from_all_local_navigables();
}

// AD-HOC: The document of the local navigable that stood beside the content navigable's RemoteNavigable activated,
//         or stands in for the next document after the host went away: it is the content navigable from now on, and
//         the navigable standing for the document hosted elsewhere is done with.
void NavigableContainer::swap_content_navigable_to_local(Badge<Page>, LocalNavigable& navigable)
{
    auto& remote_navigable = as<RemoteNavigable>(*m_content_navigable);
    VERIFY(remote_navigable.provisional_navigable().ptr() == &navigable);

    m_content_navigable = navigable;
    if (auto* layout_node = unsafe_layout_node())
        layout_node->refresh_dom_paint_facts();
    set_needs_repaint();

    remote_navigable.set_container({}, nullptr);
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#completely-finish-loading
void NavigableContainer::content_navigable_completely_finished_loading()
{
    // NB: container is this element.
    // 4. If container is an iframe element, then queue an element task on the DOM manipulation task source given container to run the iframe load event steps given container.
    if (is<HTMLIFrameElement>(*this)) {
        queue_an_element_task(Task::Source::DOMManipulation, [this] {
            run_iframe_load_event_steps(static_cast<HTMLIFrameElement&>(*this));
        });
    }
    // 5. Otherwise, if container is non-null, then queue an element task on the DOM manipulation task source given container to fire an event named load at container.
    else {
        queue_an_element_task(Task::Source::DOMManipulation, [this] {
            dispatch_event(DOM::Event::create(EventNames::load, HighResolutionTime::current_high_resolution_time(relevant_global_object(*this))));
        });
    }

    // AD-HOC: Finishing a child document can unblock its parent's load-event-delay phase, so wake the parent parser end
    //         state after queueing the container's load event.
    document().schedule_html_parser_end_check();
}

// https://html.spec.whatwg.org/multipage/iframe-embed-object.html#potentially-delays-the-load-event
bool NavigableContainer::currently_delays_the_load_event() const
{
    if (!content_navigable_has_session_history_entry_and_ready_for_navigation())
        return true;

    if (!m_potentially_delays_the_load_event)
        return false;

    // If an element type potentially delays the load event, then for each element element of that type,
    // the user agent must delay the load event of element's node document if element's content navigable is non-null
    // and any of the following are true:
    if (!m_content_navigable)
        return false;

    return m_content_navigable->delays_the_load_event_of_its_container();
}

// Clips rect to the document's viewport and to every overflow-clipping containing block above layout_node.
static CSSPixelRect visible_part_in_document_viewport(Layout::Node const& layout_node, CSSPixelRect rect)
{
    auto visible = rect.intersected(CSSPixelRect { {}, layout_node.document().viewport_rect().size() });
    for (auto const* ancestor = layout_node.containing_block(); ancestor && Painting::has_committed_box(*ancestor); ancestor = ancestor->containing_block()) {
        if (ancestor->overflow_x() == CSS::Overflow::Visible && ancestor->overflow_y() == CSS::Overflow::Visible)
            continue;
        visible.intersect(Painting::transform_rect_to_viewport(*ancestor, Painting::absolute_padding_box_rect(*ancestor)));
    }
    return visible;
}

// The bounding box, in the coordinates of the content box's document, of a rect in that document's viewport.
static CSSPixelRect transform_rect_to_local(Layout::Node const& layout_node, CSSPixelRect const& rect)
{
    auto top_left = Painting::transform_to_local_coordinates(layout_node, rect.top_left());
    auto bottom_right = top_left;
    for (auto corner : { rect.top_right(), rect.bottom_left(), rect.bottom_right() }) {
        auto local = Painting::transform_to_local_coordinates(layout_node, corner);
        top_left = { min(top_left.x(), local.x()), min(top_left.y(), local.y()) };
        bottom_right = { max(bottom_right.x(), local.x()), max(bottom_right.y(), local.y()) };
    }
    return { top_left, { bottom_right.x() - top_left.x(), bottom_right.y() - top_left.y() } };
}

void NavigableContainer::report_content_navigable_viewport_rect()
{
    if (!m_content_navigable)
        return;
    auto const* layout_node = this->layout_node();
    if (!layout_node || !Painting::is_navigable_container_viewport_paintable(*layout_node))
        return;

    // The content navigable's viewport is the container's content box, which keeps its size under the transforms
    // above the container. Those place its origin in the viewport of the local root through the viewports of the
    // documents between them, each of which shows only part of it.
    auto content_box = Painting::absolute_rect(*layout_node);
    auto origin = Painting::transform_rect_to_viewport(*layout_node, { content_box.location(), {} }).location();
    auto visible = visible_part_in_document_viewport(*layout_node, Painting::transform_rect_to_viewport(*layout_node, content_box));
    Vector<Layout::Node const*> containers { layout_node };
    auto navigable = document().navigable();
    for (; navigable && !navigable->is_local_root();) {
        auto container = navigable->container();
        auto const* container_layout_node = container ? container->layout_node() : nullptr;
        if (!container_layout_node || !Painting::is_navigable_container_viewport_paintable(*container_layout_node))
            return;
        // The content box's origin is the origin of the child's viewport, and the transforms above the container
        // apply to the child as they do to the container.
        auto container_position = Painting::absolute_position(*container_layout_node);
        origin = Painting::transform_rect_to_viewport(*container_layout_node, { origin.translated(container_position), {} }).location();
        visible = Painting::transform_rect_to_viewport(*container_layout_node, visible.translated(container_position));
        auto container_rect = Painting::transform_rect_to_viewport(*container_layout_node, Painting::absolute_rect(*container_layout_node));
        visible.intersect(visible_part_in_document_viewport(*container_layout_node, container_rect));
        containers.append(container_layout_node);
        navigable = container->document().navigable();
    }
    if (!navigable)
        return;

    // The local root itself shows only what the top-level viewport shows of it.
    if (auto intersection = navigable->viewport_intersection(); intersection.has_value())
        visible.intersect(*intersection);

    // The visible part is the content's to intersect with, so it goes back through the transforms into the content box.
    for (auto const* container_layout_node : containers.in_reverse())
        visible = transform_rect_to_local(*container_layout_node, visible).translated(-Painting::absolute_position(*container_layout_node));
    visible.intersect({ {}, content_box.size() });

    // The report is in the device pixels the content is laid out in, so a change of zoom level reports again.
    auto& page = document().page();
    ReportedContentNavigableViewport reported { page.css_to_device_rect({ origin, content_box.size() }), page.css_to_device_rect(visible) };
    if (m_reported_content_navigable_viewport == reported)
        return;
    m_reported_content_navigable_viewport = reported;
    page.client().page_did_update_child_frame_viewport(m_content_navigable->id(), reported.rect, reported.intersection);
}

ReplicatedContainerState NavigableContainer::replicated_container_state()
{
    ReplicatedContainerState state;
    state.is_in_document_tree = document().is_ancestor_of(*this);
    if (auto const* iframe = as_if<HTMLIFrameElement>(*this))
        state.iframe_sandboxing_flag_set = iframe->iframe_sandboxing_flag_set();
    state.document_active_sandboxing_flag_set = document().active_sandboxing_flag_set();
    state.local_name = local_name();
    state.iframe_referrer_policy = determine_iframe_element_referrer_policy(*this);
    return state;
}

bool NavigableContainer::content_navigable_has_session_history_entry_and_ready_for_navigation() const
{
    if (!content_navigable())
        return false;
    return m_content_navigable->has_session_history_entry_and_ready_for_navigation();
}

void NavigableContainer::set_potentially_delays_the_load_event(bool value)
{
    m_potentially_delays_the_load_event = value;
    if (!value)
        document().schedule_html_parser_end_check();
}

}
