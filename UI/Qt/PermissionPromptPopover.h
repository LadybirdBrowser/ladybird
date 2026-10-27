/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Vector.h>
#include <LibWebCommon/Permissions/Permissions.h>
#include <LibWebView/ViewImplementation.h>
#include <UI/Qt/Popover.h>

class QKeyEvent;
class QLabel;
class QShowEvent;

namespace Ladybird {

// Asks the user if a site may use a permission. Choosing one of its buttons answers the prompt; leaving it any other
// way (Escape, clicking outside it) dismisses it.
class PermissionPromptPopover final : public Popover {
public:
    AK_ALLOC_WITH_KMALLOC;

    PermissionPromptPopover(QWidget* parent, WebView::PermissionPromptRequest const&);

    WebView::PermissionPromptRequest const& request() const { return m_request; }

    void update_chrome_style(QPalette const&);

    // Hides the prompt while its tab isn't the selected one, without answering it.
    void suspend();
    void resume();

    // The tab or its document went away with the prompt unanswered.
    void ignore();

    // Closes the prompt without an answer, for a request that was settled elsewhere.
    void withdraw();

    Function<void(Web::Permissions::PromptOutcome)> on_outcome;
    // Runs each time the prompt comes up, so whoever shows it can place it by its anchor.
    Function<void()> on_shown;

private:
    virtual void keyPressEvent(QKeyEvent*) override;
    virtual void showEvent(QShowEvent*) override;
    virtual void hideEvent(QHideEvent*) override;

    void finish(Web::Permissions::PromptOutcome);
    void update_icon();

    WebView::PermissionPromptRequest m_request;
    QLabel* m_icon_tile { nullptr };
    bool m_finished { false };
    bool m_suspended { false };
};

// Shows one permission prompt at a time, in the order the requests arrived.
class PermissionPromptQueue {
    AK_MAKE_NONCOPYABLE(PermissionPromptQueue);
    AK_MAKE_NONMOVABLE(PermissionPromptQueue);

public:
    PermissionPromptQueue() = default;
    ~PermissionPromptQueue();

    Function<PermissionPromptPopover*(WebView::PermissionPromptRequest const&)> create_popover;

    void enqueue(WebView::PermissionPromptRequest, WebView::PermissionPromptCallback);
    void cancel(WebView::PermissionPromptRequest const&);
    void ignore_all();

    void suspend();
    void resume();

    PermissionPromptPopover* current() const { return m_current; }

private:
    struct Pending {
        WebView::PermissionPromptRequest request;
        WebView::PermissionPromptCallback on_outcome;
    };

    void show_next();

    Vector<Pending> m_pending;
    PermissionPromptPopover* m_current { nullptr };
    WebView::PermissionPromptCallback m_current_callback;
    bool m_suspended { false };
};

}
