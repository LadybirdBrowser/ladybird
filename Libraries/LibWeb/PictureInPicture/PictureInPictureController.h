/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>
#include <LibGfx/Size.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PictureInPicture/PictureInPictureWindow.h>

namespace Web::PictureInPicture {

// Runs the steps that the spec enqueues to a traversable navigable's picture-in-picture parallel queue, and tracks
// the window that the UI process shows for this page. The UI shows a single window, so a page has at most one open.
class WEB_API PictureInPictureController final : public GC::Cell {
    GC_CELL(PictureInPictureController, GC::Cell);
    GC_DECLARE_ALLOCATOR(PictureInPictureController);

public:
    bool has_picture_in_picture_support() const { return m_has_picture_in_picture_support; }
    void set_has_picture_in_picture_support(bool has_support) { m_has_picture_in_picture_support = has_support; }

    void enqueue_request(HTML::HTMLVideoElement&, WebIDL::Promise&);
    void enqueue_exit(DOM::Document&, WebIDL::Promise&);
    void enqueue_close_window(PictureInPictureWindow&);

    void did_open_window(Gfx::IntSize window_size);
    void window_did_resize(Gfx::IntSize);
    void window_did_close();

    bool is_waiting_for_window_to_open() const { return m_window_awaiting_open.has_value(); }
    bool has_open_window() const { return m_open_window != nullptr; }

private:
    explicit PictureInPictureController(Page&);

    virtual void visit_edges(Cell::Visitor&) override;

    struct Request {
        GC::Ref<HTML::HTMLVideoElement> video;
        GC::Ref<WebIDL::Promise> promise;

        void visit_edges(GC::Cell::Visitor& visitor)
        {
            visitor.visit(video);
            visitor.visit(promise);
        }
    };

    struct Exit {
        GC::Ref<DOM::Document> document;
        GC::Ref<WebIDL::Promise> promise;

        void visit_edges(GC::Cell::Visitor& visitor)
        {
            visitor.visit(document);
            visitor.visit(promise);
        }
    };

    struct CloseWindow {
        GC::Ref<PictureInPictureWindow> window;

        void visit_edges(GC::Cell::Visitor& visitor) { visitor.visit(window); }
    };

    struct UserInterfaceClosedWindow {
        GC::Ref<PictureInPictureWindow> window;

        void visit_edges(GC::Cell::Visitor& visitor) { visitor.visit(window); }
    };

    using Operation = Variant<Request, Exit, CloseWindow, UserInterfaceClosedWindow>;

    struct WindowAwaitingOpen {
        Request request;
        GC::Ref<HTML::LocalTraversableNavigable> traversable;

        void visit_edges(GC::Cell::Visitor&);
    };

    enum class WindowIsOpenInUserInterface : bool {
        No,
        Yes,
    };

    void enqueue(Operation);
    void process_pending_operations();

    void run_request_steps(Request const&);
    void run_exit_steps(DOM::Document&, GC::Ptr<WebIDL::Promise>, WindowIsOpenInUserInterface);
    void close_window(PictureInPictureWindow&, WindowIsOpenInUserInterface);

    GC::Ref<Page> m_page;
    bool m_has_picture_in_picture_support { false };

    Vector<Operation> m_pending_operations;
    Optional<WindowAwaitingOpen> m_window_awaiting_open;

    GC::Ptr<PictureInPictureWindow> m_open_window;
};

void run_unloading_cleanup_steps(DOM::Document&);

}
