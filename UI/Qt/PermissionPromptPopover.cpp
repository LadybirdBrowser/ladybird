/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/PermissionPromptPopover.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/Theme.h>

#include <QHBoxLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QShowEvent>
#include <QVBoxLayout>

namespace Ladybird {

static constexpr int PERMISSION_PROMPT_WIDTH = 352;

static QString requester_text(WebView::PermissionPromptRequest const& request)
{
    if (request.is_file)
        return QStringLiteral("This file");
    if (request.top_level_origin.is_opaque())
        return QStringLiteral("This page");

    auto host = qstring_from_ak_string(request.top_level_origin.host().serialize());
    if (auto port = request.top_level_origin.port(); port.has_value())
        host = QStringLiteral("%1:%2").arg(host).arg(*port);
    return host;
}

static QString feature_text(Web::Permissions::PermissionName name)
{
    switch (name) {
    case Web::Permissions::PermissionName::Geolocation:
        return QStringLiteral("know your location");
    case Web::Permissions::PermissionName::Microphone:
        return QStringLiteral("use your microphone");
    }
    VERIFY_NOT_REACHED();
}

static ChromeIcon feature_icon(Web::Permissions::PermissionName name)
{
    switch (name) {
    case Web::Permissions::PermissionName::Geolocation:
        return ChromeIcon::Location;
    case Web::Permissions::PermissionName::Microphone:
        return ChromeIcon::Microphone;
    }
    VERIFY_NOT_REACHED();
}

// The prompt is laid out like a message popover: the permission's icon in a tile before the question, and the answers
// below. All three answers are secondary buttons, since the browser recommends none of them.
PermissionPromptPopover::PermissionPromptPopover(QWidget* parent, WebView::PermissionPromptRequest const& request)
    : Popover(parent)
    , m_request(request)
{
    setAccessibleName("Permission request");
    card().setObjectName("LadybirdPermissionPrompt");
    card().setFixedWidth(PERMISSION_PROMPT_WIDTH);

    auto* layout = new QVBoxLayout(&card());
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(6);

    auto* header = new QHBoxLayout;
    header->setSpacing(12);
    m_icon_tile = new QLabel(&card());
    m_icon_tile->setObjectName("LadybirdPermissionPromptIcon");
    m_icon_tile->setFixedSize(32, 32);
    m_icon_tile->setAlignment(Qt::AlignCenter);
    header->addWidget(m_icon_tile, 0, Qt::AlignTop);
    auto* title = new QLabel(QStringLiteral("%1 wants to %2").arg(requester_text(request), feature_text(request.name)), &card());
    title->setObjectName("LadybirdPermissionPromptTitle");
    title->setTextFormat(Qt::PlainText);
    title->setWordWrap(true);
    header->addWidget(title, 1);
    layout->addLayout(header);

    auto* hint = new QLabel(QStringLiteral("macOS will then ask you to let Ladybird use your location."), &card());
    hint->setObjectName("LadybirdPermissionPromptHint");
    hint->setWordWrap(true);
    hint->setVisible(request.show_os_hint);
    layout->addWidget(hint);

    layout->addSpacing(8);
    auto add_button = [&](char const* name, QString const& text, Web::Permissions::PromptOutcome outcome) {
        auto* button = new QPushButton(text, &card());
        button->setObjectName(name);
        // NB: The platform style lays out a push button by a frame of its own rather than by the button, which would
        //     make it overlap its neighbors.
        button->setAttribute(Qt::WA_LayoutUsesWidgetRect);
        QObject::connect(button, &QPushButton::clicked, this, [this, outcome] { finish(outcome); });
        layout->addWidget(button);
    };
    add_button("LadybirdPermissionPromptAllowAlways", QStringLiteral("Allow while visiting the site"), Web::Permissions::PromptOutcome::AllowAlways);
    add_button("LadybirdPermissionPromptAllowOnce", QStringLiteral("Allow this time"), Web::Permissions::PromptOutcome::AllowOnce);
    add_button("LadybirdPermissionPromptBlock", QStringLiteral("Never allow"), Web::Permissions::PromptOutcome::Block);

    update_icon();
    adjustSize();
}

void PermissionPromptPopover::update_chrome_style(QPalette const& palette)
{
    setPalette(palette);
    setStyleSheet(ChromeStyle::permission_prompt_style_sheet(palette));
    update_icon();
}

void PermissionPromptPopover::update_icon()
{
    m_icon_tile->setPixmap(create_chrome_icon(feature_icon(m_request.name), Theme::panel().link).pixmap(QSize(18, 18), devicePixelRatioF()));
}

void PermissionPromptPopover::suspend()
{
    m_suspended = true;
    hide();
}

void PermissionPromptPopover::resume()
{
    m_suspended = false;
    if (!m_finished)
        show();
}

void PermissionPromptPopover::ignore()
{
    finish(Web::Permissions::PromptOutcome::Ignored);
}

void PermissionPromptPopover::withdraw()
{
    m_finished = true;
    hide();
}

void PermissionPromptPopover::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape) {
        finish(Web::Permissions::PromptOutcome::Dismissed);
        return;
    }
    Popover::keyPressEvent(event);
}

void PermissionPromptPopover::showEvent(QShowEvent* event)
{
    Popover::showEvent(event);
    if (on_shown)
        on_shown();
}

void PermissionPromptPopover::hideEvent(QHideEvent* event)
{
    Popover::hideEvent(event);

    // A popover hides itself when the user clicks outside it, which dismisses the prompt.
    if (!m_suspended)
        finish(Web::Permissions::PromptOutcome::Dismissed);
}

void PermissionPromptPopover::finish(Web::Permissions::PromptOutcome outcome)
{
    if (m_finished)
        return;
    m_finished = true;
    hide();
    if (on_outcome)
        on_outcome(outcome);
}

PermissionPromptQueue::~PermissionPromptQueue()
{
    if (m_current) {
        m_current->on_outcome = nullptr;
        m_current->deleteLater();
    }
}

void PermissionPromptQueue::enqueue(WebView::PermissionPromptRequest request, WebView::PermissionPromptCallback on_outcome)
{
    m_pending.append({ move(request), move(on_outcome) });
    if (!m_current)
        show_next();
}

void PermissionPromptQueue::cancel(WebView::PermissionPromptRequest const& request)
{
    m_pending.remove_all_matching([&](auto const& pending) { return pending.request.is_same_request_as(request); });

    if (!m_current || !m_current->request().is_same_request_as(request))
        return;
    auto* popover = exchange(m_current, nullptr);
    popover->on_outcome = nullptr;
    m_current_callback = nullptr;
    popover->withdraw();
    popover->deleteLater();
    show_next();
}

void PermissionPromptQueue::ignore_all()
{
    auto pending = move(m_pending);
    if (m_current)
        m_current->ignore();
    for (auto& entry : pending)
        entry.on_outcome(Web::Permissions::PromptOutcome::Ignored);
}

void PermissionPromptQueue::suspend()
{
    m_suspended = true;
    if (m_current)
        m_current->suspend();
}

void PermissionPromptQueue::resume()
{
    m_suspended = false;
    if (m_current)
        m_current->resume();
}

void PermissionPromptQueue::show_next()
{
    if (m_pending.is_empty() || !create_popover)
        return;

    auto next = m_pending.take_first();
    m_current = create_popover(next.request);
    m_current_callback = move(next.on_outcome);
    m_current->on_outcome = [this](Web::Permissions::PromptOutcome outcome) {
        auto* popover = exchange(m_current, nullptr);
        auto callback = move(m_current_callback);
        popover->deleteLater();
        callback(outcome);
        show_next();
    };

    if (m_suspended)
        m_current->suspend();
    else
        m_current->show();
}

}
