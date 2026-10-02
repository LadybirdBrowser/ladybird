/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/HTML/LocalTraversableNavigable.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/PictureInPicture/PictureInPictureController.h>
#include <LibWeb/PictureInPicture/PictureInPictureEvent.h>
#include <LibWeb/PictureInPicture/PictureInPictureWindow.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::PictureInPicture {

GC_DEFINE_ALLOCATOR(PictureInPictureController);

PictureInPictureController::PictureInPictureController(Page& page)
    : m_page(page)
{
}

void PictureInPictureController::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_page);
    visitor.visit(m_pending_operations);
    visitor.visit(m_window_awaiting_open);
    visitor.visit(m_open_window);
}

void PictureInPictureController::WindowAwaitingOpen::visit_edges(GC::Cell::Visitor& visitor)
{
    request.visit_edges(visitor);
    visitor.visit(traversable);
}

static void resolve_promise(WebIDL::Promise& promise, GC::Ptr<PictureInPictureWindow> window = nullptr)
{
    auto& realm = WebIDL::promise_realm(promise);
    HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
    if (window)
        WebIDL::resolve_promise(promise, Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, GC::Ref { *window }));
    else
        WebIDL::resolve_promise(promise);
}

static void reject_promise_with_invalid_state_error(WebIDL::Promise& promise, Utf16String const& message)
{
    auto& realm = WebIDL::promise_realm(promise);
    HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
    WebIDL::reject_promise(promise, WebIDL::InvalidStateError::create(message));
}

static void fire_picture_in_picture_event(DOM::Element& element, Utf16FlyString const& event_name, PictureInPictureWindow& window)
{
    PictureInPictureEventInit event_init { { .bubbles = true }, window };
    auto event = PictureInPictureEvent::create(event_name, event_init, HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(element)));
    element.dispatch_event(event);
}

void PictureInPictureController::enqueue_request(HTML::HTMLVideoElement& video, WebIDL::Promise& promise)
{
    enqueue(Request { video, promise });
}

void PictureInPictureController::enqueue_exit(DOM::Document& document, GC::Ptr<WebIDL::Promise> promise)
{
    enqueue(Exit { document, promise });
}

void PictureInPictureController::enqueue_close_window(PictureInPictureWindow& window)
{
    enqueue(CloseWindow { window });
}

void PictureInPictureController::enqueue(Operation operation)
{
    m_pending_operations.append(move(operation));
    process_pending_operations();
}

void PictureInPictureController::process_pending_operations()
{
    // A request suspends the queue until the UI process has opened the window for it.
    while (!m_window_awaiting_open.has_value() && !m_pending_operations.is_empty()) {
        auto operation = m_pending_operations.take_first();
        operation.visit(
            [&](Request const& request) {
                run_request_steps(request);
            },
            [&](Exit const& exit) {
                // https://w3c.github.io/picture-in-picture/#dom-document-exitpictureinpicture
                // AD-HOC: These are the parallel steps of exitPictureInPicture() in the overhaul proposed in
                //         https://github.com/w3c/picture-in-picture/pull/260.
                // 1. If this's Picture-in-Picture element is null:
                if (!exit.document->picture_in_picture_element()) {
                    // 1. Queue a global task on the media element event task source given this's relevant global
                    //    object to resolve p.
                    // AD-HOC: There is no media element to take the task source from, so this uses the DOM
                    //         manipulation task source. The promise is rejected, as the overhaul's reviewers asked.
                    if (exit.promise) {
                        HTML::queue_global_task(HTML::Task::Source::DOMManipulation, HTML::relevant_global_object(*exit.document), GC::create_function(GC::Heap::the(), [promise = GC::Ref { *exit.promise }] {
                            reject_promise_with_invalid_state_error(promise, "There is no Picture-in-Picture element to exit"_utf16);
                        }));
                    }

                    // 2. Return.
                    return;
                }

                // 2. Run the exit Picture-in-Picture algorithm given this and p.
                run_exit_steps(exit.document, exit.promise, WindowIsOpenInUserInterface::Yes);
            },
            [&](CloseWindow const& close_window) {
                // https://w3c.github.io/picture-in-picture/#unloading-steps
                // 1. Run the close window algorithm with window.
                this->close_window(close_window.window, WindowIsOpenInUserInterface::Yes);
            },
            [&](UserInterfaceClosedWindow const& closed_window) {
                auto& window = *closed_window.window;
                if (!window.is_open())
                    return;

                // AD-HOC: The spec leaves closing the window to the user agent. When the user closes it, or the UI
                //         process gives it to another page, run the exit algorithm for the document it belongs to.
                auto& video = window.video();
                auto& document = video.document();
                if (video.is_picture_in_picture_element())
                    run_exit_steps(document, nullptr, WindowIsOpenInUserInterface::No);
                else
                    this->close_window(window, WindowIsOpenInUserInterface::No);
            });
    }
}

// https://w3c.github.io/picture-in-picture/#request-picture-in-picture
void PictureInPictureController::run_request_steps(Request const& request)
{
    auto& video = *request.video;

    // NB: Nothing is left to do for a request whose promise the disable Picture-in-Picture steps rejected.
    if (!video.has_pending_picture_in_picture_promise(request.promise))
        return;

    // 1. If this is doc's Picture-in-Picture element:
    if (video.is_picture_in_picture_element()) {
        // 1. Queue a global task on the media element event task source given global to resolve p with the
        //    Picture-in-Picture window associated with this.
        HTML::queue_global_task(video.media_element_event_task_source(), HTML::relevant_global_object(video), GC::create_function(GC::Heap::the(), [video = request.video, promise = request.promise] {
            if (video->take_pending_picture_in_picture_promise(promise))
                resolve_promise(promise, video->picture_in_picture_window());
        }));

        // 2. Abort these steps.
        return;
    }

    // 2. Attempt to associate a Picture-in-Picture window with this.
    // NB: The window shows a page of its own, which the UI process creates along with the window.
    HTML::WebViewHints hints;
    hints.picture_in_picture_video_size = Gfx::Size<u32> { video.video_width(), video.video_height() }.to_type<int>();
    auto new_web_view = m_page->client().page_did_request_new_web_view(HTML::ActivateTab::No, hints, {}, {}, {}, {});

    // 3. If the previous step failed:
    if (!new_web_view.page) {
        // 1. Queue a global task on the media element event task source given global to reject p with
        //    InvalidStateError DOMException.
        HTML::queue_global_task(video.media_element_event_task_source(), HTML::relevant_global_object(video), GC::create_function(GC::Heap::the(), [video = request.video, promise = request.promise] {
            if (video->take_pending_picture_in_picture_promise(promise))
                reject_promise_with_invalid_state_error(promise, "Unable to open a Picture-in-Picture window"_utf16);
        }));

        // 2. Abort these steps.
        return;
    }

    auto traversable = HTML::LocalTraversableNavigable::create_for_new_web_view(move(new_web_view), nullptr);
    m_window_awaiting_open = WindowAwaitingOpen { request, traversable };
}

void PictureInPictureController::did_open_window(Gfx::IntSize window_size)
{
    if (!m_window_awaiting_open.has_value())
        return;

    // https://w3c.github.io/picture-in-picture/#request-picture-in-picture
    auto [request, traversable] = m_window_awaiting_open.release_value();
    auto& video = *request.video;
    auto& document = video.document();
    auto& global = HTML::relevant_global_object(video);

    // 4. Let pipWindow be a new instance of PictureInPictureWindow that represents this's associated
    //    Picture-in-Picture window.
    auto picture_in_picture_window = PictureInPictureWindow::create(video, window_size, traversable);

    // NB: The UI process opened the new window in place of any window this page had open. When that window belongs
    //     to doc's Picture-in-Picture element, the next step exits it.
    if (m_open_window) {
        auto& previous_video = m_open_window->video();
        auto& previous_document = previous_video.document();

        // AD-HOC: The spec assumes that each document has its own window. When another document's
        //         Picture-in-Picture element loses its window, run the exit algorithm for that document too.
        if (&previous_document != &document && previous_video.is_picture_in_picture_element())
            run_exit_steps(previous_document, nullptr, WindowIsOpenInUserInterface::No);
        else
            close_window(*m_open_window, WindowIsOpenInUserInterface::No);
    }

    // AD-HOC: This follows the overhaul proposed in https://github.com/w3c/picture-in-picture/pull/260, which runs
    //         the exit algorithm here rather than in the task below, so that leavepictureinpicture fires first.
    // 5. If doc's Picture-in-Picture element is not null, run the exit Picture-in-Picture algorithm given doc and
    //    null.
    if (document.picture_in_picture_element())
        run_exit_steps(document, nullptr, WindowIsOpenInUserInterface::No);

    video.set_picture_in_picture_window(picture_in_picture_window);
    m_open_window = picture_in_picture_window;

    // NB: A document that stopped being fully active while the window opened, such as by unloading, never runs the
    //     task below, so the window would stay open without entering Picture-in-Picture. It is closed again instead.
    if (!document.is_fully_active()) {
        close_window(picture_in_picture_window, WindowIsOpenInUserInterface::Yes);
        process_pending_operations();
        return;
    }

    // 6. Queue a global task on the media element event task source given global, to perform the following steps:
    HTML::queue_global_task(video.media_element_event_task_source(), global, GC::create_function(GC::Heap::the(), [this, video = request.video, promise = request.promise, picture_in_picture_window] {
        auto& document = video->document();

        // NB: When the disable Picture-in-Picture steps rejected p while the window opened, it is closed again
        //     rather than entered.
        if (!video->take_pending_picture_in_picture_promise(promise)) {
            close_window(picture_in_picture_window, WindowIsOpenInUserInterface::Yes);
            return;
        }

        // 1. Set doc's Picture-in-Picture element to this.
        document.set_picture_in_picture_element(video);

        // 2. Append relevant settings object's origin to initiators of active Picture-in-Picture sessions.
        // NB: Nothing reads this list, and https://github.com/w3c/picture-in-picture/issues/263 proposes its removal.

        // 3. If this is fullscreenElement, then exit fullscreen.
        // FIXME: Exit fullscreen.

        // 4. Fire an event named enterpictureinpicture using PictureInPictureEvent at this with its bubbles attribute
        //    initialized to true and its pictureInPictureWindow attribute initialized to Picture-in-Picture window.
        fire_picture_in_picture_event(video, HTML::EventNames::enterpictureinpicture, picture_in_picture_window);

        // 5. Resolve p with pipWindow.
        resolve_promise(promise, picture_in_picture_window);
    }));

    process_pending_operations();
}

// https://w3c.github.io/picture-in-picture/#exit-picture-in-picture-algorithm
// AD-HOC: This follows the overhaul proposed in https://github.com/w3c/picture-in-picture/pull/260. The current text
//         fires leavepictureinpicture from a separately queued task, and does not say which document to exit. See
//         https://github.com/w3c/picture-in-picture/issues/252.
void PictureInPictureController::run_exit_steps(DOM::Document& document, GC::Ptr<WebIDL::Promise> promise, WindowIsOpenInUserInterface window_is_open_in_user_interface)
{
    // 1. Assert that these steps are running on the picture-in-picture parallel queue.

    // 2. Let global be doc's relevant global object.
    auto& global = HTML::relevant_global_object(document);

    // 3. Run the close window algorithm with the Picture-in-Picture window associated with doc's Picture-in-Picture
    //    element.
    auto& video = as<HTML::HTMLVideoElement>(*document.picture_in_picture_element());
    close_window(*video.picture_in_picture_window(), window_is_open_in_user_interface);

    // 4. Queue a global task on the media element event task source given global, to perform the following steps:
    HTML::queue_global_task(video.media_element_event_task_source(), global, GC::create_function(GC::Heap::the(), [document = GC::Ref { document }, promise] {
        // 1. If doc is not fully active or doc's Picture-in-Picture element is null:
        if (!document->is_fully_active() || !document->picture_in_picture_element()) {
            // 1. If p is not null, resolve p with undefined.
            // AD-HOC: Reject p instead, as the overhaul's reviewers asked.
            if (promise)
                reject_promise_with_invalid_state_error(*promise, "There is no Picture-in-Picture element to exit"_utf16);

            // 2. Return.
            return;
        }

        // 2. Let element be doc's Picture-in-Picture element.
        auto& element = as<HTML::HTMLVideoElement>(*document->picture_in_picture_element());

        // 3. Set doc's Picture-in-Picture element to null.
        document->set_picture_in_picture_element(nullptr);

        // 4. Fire an event named leavepictureinpicture using PictureInPictureEvent at the element with its bubbles
        //    attribute initialized to true and its pictureInPictureWindow attribute initialized to Picture-in-Picture
        //    window associated with element.
        fire_picture_in_picture_event(element, HTML::EventNames::leavepictureinpicture, *element.picture_in_picture_window());

        // 5. Remove one item matching relevant settings object's origin from initiators of active Picture-in-Picture
        //    sessions.
        // NB: Nothing reads this list, and https://github.com/w3c/picture-in-picture/issues/263 proposes its removal.

        // 6. If p is not null, resolve p with undefined.
        if (promise)
            resolve_promise(*promise);
    }));
}

// https://w3c.github.io/picture-in-picture/#close-window-algorithm
void PictureInPictureController::close_window(PictureInPictureWindow& window, WindowIsOpenInUserInterface window_is_open_in_user_interface)
{
    if (window.is_open()) {
        window.close();
        window.traversable().close_top_level_traversable(HTML::LocalTraversableNavigable::PromptToUnload::No);
    }

    if (m_open_window.ptr() != &window)
        return;
    m_open_window = nullptr;
    if (window_is_open_in_user_interface == WindowIsOpenInUserInterface::Yes)
        m_page->client().page_did_exit_picture_in_picture();
}

void PictureInPictureController::window_did_resize(Gfx::IntSize window_size)
{
    if (!m_open_window)
        return;
    m_open_window->set_size(window_size);

    // https://w3c.github.io/picture-in-picture/#interface-picture-in-picture-window
    // When the size of a Picture-in-Picture window pipWindow changes, the user agent MUST queue a task to fire an
    // event named resize at pipWindow.
    auto& video = m_open_window->video();
    HTML::queue_global_task(video.media_element_event_task_source(), HTML::relevant_global_object(video), GC::create_function(GC::Heap::the(), [window = GC::Ref { *m_open_window }] {
        window->dispatch_event(DOM::Event::create(HTML::relevant_global_object(window->video()), HTML::EventNames::resize));
    }));
}

void PictureInPictureController::window_did_close()
{
    if (m_open_window)
        enqueue(UserInterfaceClosedWindow { *m_open_window });
}

// https://w3c.github.io/picture-in-picture/#unloading-steps
// AD-HOC: These are the unloading steps from the overhaul proposed in
//         https://github.com/w3c/picture-in-picture/pull/260. The current text runs the exit algorithm, which throws
//         when there is no Picture-in-Picture element.
void run_unloading_cleanup_steps(DOM::Document& document)
{
    // 1. If doc's Picture-in-Picture element is null, return.
    auto element = document.picture_in_picture_element();
    if (!element)
        return;

    // 2. Let window be the Picture-in-Picture window associated with doc's Picture-in-Picture element.
    auto window = as<HTML::HTMLVideoElement>(*element).picture_in_picture_window();

    // 3. Set doc's Picture-in-Picture element to null.
    document.set_picture_in_picture_element(nullptr);

    // 4. Remove one item matching relevant settings object's origin from initiators of active Picture-in-Picture
    //    sessions.
    // NB: Nothing reads this list, and https://github.com/w3c/picture-in-picture/issues/263 proposes its removal.

    // 5. Enqueue the following steps to doc's picture-in-picture parallel queue:
    document.page().picture_in_picture_controller().enqueue_close_window(*window);
}

}
