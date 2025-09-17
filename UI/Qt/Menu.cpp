/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/Icon.h>
#include <UI/Qt/Menu.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/WebContentView.h>

#include <QAction>
#include <QMenu>
#include <QPointer>
#include <QWidget>

namespace Ladybird {

class ActionObserver final : public WebView::Action::Observer {
public:
    static NonnullOwnPtr<ActionObserver> create(WebView::Action& action, QAction& qaction)
    {
        return adopt_own(*new ActionObserver(action, qaction));
    }

    virtual void on_text_changed(WebView::Action& action) override
    {
        if (m_action)
            m_action->setText(qstring_from_ak_string(action.text()));
    }

    virtual void on_tooltip_changed(WebView::Action& action) override
    {
        if (m_action)
            m_action->setToolTip(qstring_from_ak_string(action.tooltip()));
    }

    virtual void on_enabled_state_changed(WebView::Action& action) override
    {
        if (m_action)
            m_action->setEnabled(action.enabled());
    }

    virtual void on_visible_state_changed(WebView::Action& action) override
    {
        if (m_action)
            m_action->setVisible(action.visible());
    }

    virtual void on_checked_state_changed(WebView::Action& action) override
    {
        if (m_action)
            m_action->setChecked(action.checked());
    }

private:
    ActionObserver(WebView::Action& action, QAction& qaction)
        : m_action(&qaction)
    {
        QObject::connect(m_action, &QAction::triggered, [weak_action = action.make_weak_ptr()](bool checked) {
            if (auto action = weak_action.strong_ref()) {
                if (action->is_checkable())
                    action->set_checked(checked);
                action->activate();
            }
        });
        QObject::connect(m_action->parent(), &QObject::destroyed, [this, weak_action = action.make_weak_ptr()]() {
            if (auto action = weak_action.strong_ref())
                action->remove_observer(*this);
        });
    }

    QPointer<QAction> m_action;
};

static void initialize_native_control(WebView::Action& action, QAction& qaction, QPalette const& palette)
{
    switch (action.id()) {
    case WebView::ActionID::NavigateBack:
        qaction.setIcon(create_tvg_icon_with_theme_colors("back", palette));
        qaction.setShortcut(QKeySequence::StandardKey::Back);
        break;
    case WebView::ActionID::NavigateForward:
        qaction.setIcon(create_tvg_icon_with_theme_colors("forward", palette));
        qaction.setShortcut(QKeySequence::StandardKey::Forward);
        break;
    case WebView::ActionID::Reload:
        qaction.setIcon(create_tvg_icon_with_theme_colors("reload", palette));
        qaction.setShortcuts({ QKeySequence(Qt::CTRL | Qt::Key_R), QKeySequence(Qt::Key_F5) });
        break;

    case WebView::ActionID::CopySelection:
        qaction.setIcon(load_icon_from_uri("resource://icons/16x16/edit-copy.png"sv));
        qaction.setShortcut(QKeySequence::StandardKey::Copy);
        break;
    case WebView::ActionID::Paste:
        qaction.setIcon(load_icon_from_uri("resource://icons/16x16/paste.png"sv));
        qaction.setShortcut(QKeySequence::StandardKey::Paste);
        break;
    case WebView::ActionID::SelectAll:
        qaction.setIcon(load_icon_from_uri("resource://icons/16x16/select-all.png"sv));
        qaction.setShortcut(QKeySequence::StandardKey::SelectAll);
        break;

    case WebView::ActionID::SearchSelectedText:
        qaction.setIcon(load_icon_from_uri("resource://icons/16x16/find.png"sv));
        break;

    case WebView::ActionID::OpenProcessesPage:
        qaction.setIcon(load_icon_from_uri("resource://icons/16x16/app-system-monitor.png"sv));
        qaction.setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M));
        break;
    case WebView::ActionID::OpenSettingsPage:
        qaction.setIcon(load_icon_from_uri("resource://icons/16x16/settings.png"sv));
        qaction.setShortcut(QKeySequence::StandardKey::Preferences);
        break;
    case WebView::ActionID::ToggleDevTools:
        qaction.setIcon(load_icon_from_uri("resource://icons/browser/dom-tree.png"sv));
        qaction.setShortcuts({
            QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_I),
            QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C),
            QKeySequence(Qt::Key_F12),
        });
        break;
    case WebView::ActionID::ViewSource:
        qaction.setIcon(load_icon_from_uri("resource://icons/16x16/filetype-html.png"sv));
        qaction.setShortcut(QKeySequence(Qt::CTRL | Qt::Key_U));
        break;

    case WebView::ActionID::TakeVisibleScreenshot:
    case WebView::ActionID::TakeFullScreenshot:
        qaction.setIcon(load_icon_from_uri("resource://icons/16x16/filetype-image.png"sv));
        break;

    case WebView::ActionID::OpenInNewTab:
        qaction.setIcon(load_icon_from_uri("resource://icons/16x16/new-tab.png"sv));
        break;
    case WebView::ActionID::CopyURL:
        qaction.setIcon(load_icon_from_uri("resource://icons/16x16/edit-copy.png"sv));
        break;

    case WebView::ActionID::OpenImage:
        qaction.setIcon(load_icon_from_uri("resource://icons/16x16/filetype-image.png"sv));
        break;
    case WebView::ActionID::CopyImage:
        qaction.setIcon(load_icon_from_uri("resource://icons/16x16/edit-copy.png"sv));
        break;

    case WebView::ActionID::OpenAudio:
        qaction.setIcon(load_icon_from_uri("resource://icons/16x16/filetype-sound.png"sv));
        break;
    case WebView::ActionID::OpenVideo:
        qaction.setIcon(load_icon_from_uri("resource://icons/16x16/filetype-video.png"sv));
        break;
    case WebView::ActionID::PlayMedia:
        qaction.setIcon(load_icon_from_uri("resource://icons/16x16/play.png"sv));
        break;
    case WebView::ActionID::PauseMedia:
        qaction.setIcon(load_icon_from_uri("resource://icons/16x16/pause.png"sv));
        break;
    case WebView::ActionID::MuteMedia:
        qaction.setIcon(load_icon_from_uri("resource://icons/16x16/audio-volume-muted.png"sv));
        break;
    case WebView::ActionID::UnmuteMedia:
        qaction.setIcon(load_icon_from_uri("resource://icons/16x16/audio-volume-high.png"sv));
        break;

    case WebView::ActionID::ZoomIn: {
        qaction.setIcon(load_icon_from_uri("resource://icons/16x16/zoom-in.png"sv));

        auto zoom_in_shortcuts = QKeySequence::keyBindings(QKeySequence::StandardKey::ZoomIn);
        auto secondary_zoom_in_shortcut = QKeySequence(Qt::CTRL | Qt::Key_Equal);

        if (!zoom_in_shortcuts.contains(secondary_zoom_in_shortcut))
            zoom_in_shortcuts.append(move(secondary_zoom_in_shortcut));

        qaction.setShortcuts(zoom_in_shortcuts);
        break;
    }
    case WebView::ActionID::ZoomOut:
        qaction.setIcon(load_icon_from_uri("resource://icons/16x16/zoom-out.png"sv));
        qaction.setShortcut(QKeySequence::StandardKey::ZoomOut);
        break;
    case WebView::ActionID::ResetZoom:
        qaction.setIcon(load_icon_from_uri("resource://icons/16x16/zoom-reset.png"sv));
        qaction.setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));
        break;

    case WebView::ActionID::DumpSessionHistoryTree:
        qaction.setIcon(load_icon_from_uri("resource://icons/16x16/history.png"sv));
        break;
    case WebView::ActionID::DumpDOMTree:
        qaction.setIcon(load_icon_from_uri("resource://icons/browser/dom-tree.png"sv));
        break;
    case WebView::ActionID::DumpLayoutTree:
    case WebView::ActionID::DumpPaintTree:
    case WebView::ActionID::DumpDisplayList:
        qaction.setIcon(load_icon_from_uri("resource://icons/16x16/layout.png"sv));
        break;
    case WebView::ActionID::DumpStackingContextTree:
        qaction.setIcon(load_icon_from_uri("resource://icons/16x16/layers.png"sv));
        break;
    case WebView::ActionID::DumpStyleSheets:
    case WebView::ActionID::DumpStyles:
        qaction.setIcon(load_icon_from_uri("resource://icons/16x16/filetype-css.png"sv));
        break;
    case WebView::ActionID::DumpCSSErrors:
        qaction.setIcon(load_icon_from_uri("resource://icons/16x16/error.png"sv));
        break;
    case WebView::ActionID::DumpCookies:
        qaction.setIcon(load_icon_from_uri("resource://icons/browser/cookie.png"sv));
        break;
    case WebView::ActionID::DumpLocalStorage:
        qaction.setIcon(load_icon_from_uri("resource://icons/browser/local-storage.png"sv));
        break;
    case WebView::ActionID::ShowLineBoxBorders:
        qaction.setIcon(load_icon_from_uri("resource://icons/16x16/box.png"sv));
        break;
    case WebView::ActionID::CollectGarbage:
        qaction.setIcon(load_icon_from_uri("resource://icons/16x16/trash-can.png"sv));
        qaction.setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_G));
        break;
    case WebView::ActionID::ClearCache:
        qaction.setIcon(load_icon_from_uri("resource://icons/browser/clear-cache.png"sv));
        break;

    default:
        break;
    }

    if (action.is_checkable())
        qaction.setCheckable(true);

    action.add_observer(ActionObserver::create(action, qaction));
}

static void add_items_to_menu(QMenu& menu, QWidget& parent, Span<WebView::Menu::MenuItem> menu_items)
{
    for (auto& menu_item : menu_items) {
        menu_item.visit(
            [&](NonnullRefPtr<WebView::Action>& action) {
                auto* qaction = create_application_action(parent, action);
                menu.addAction(qaction);

                if (action->id() == WebView::ActionID::SpoofUserAgent || action->id() == WebView::ActionID::NavigatorCompatibilityMode) {
                    if (menu.icon().isNull())
                        menu.setIcon(load_icon_from_uri("resource://icons/16x16/spoof.png"sv));
                }
            },
            [&](NonnullRefPtr<WebView::Menu> const& submenu) {
                auto* qsubmenu = new QMenu(qstring_from_ak_string(submenu->title()), &menu);
                add_items_to_menu(*qsubmenu, parent, submenu->items());

                menu.addMenu(qsubmenu);
            },
            [&](WebView::Separator) {
                menu.addSeparator();
            });
    }
}

QMenu* create_application_menu(QWidget& parent, WebView::Menu& menu)
{
    auto* application_menu = new QMenu(qstring_from_ak_string(menu.title()), &parent);
    add_items_to_menu(*application_menu, parent, menu.items());
    return application_menu;
}

QMenu* create_context_menu(QWidget& parent, WebContentView& view, WebView::Menu& menu)
{
    auto* application_menu = create_application_menu(parent, menu);

    menu.on_activation = [view = QPointer { &view }, application_menu = QPointer { application_menu }](Gfx::IntPoint position) {
        if (view && application_menu)
            application_menu->exec(view->map_point_to_global_position(position));
    };

    return application_menu;
}

QAction* create_application_action(QWidget& parent, WebView::Action& action)
{
    auto* qaction = new QAction(&parent);
    initialize_native_control(action, *qaction, parent.palette());
    return qaction;
}

}
