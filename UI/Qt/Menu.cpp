/*
 * Copyright (c) 2025-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
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
    static NonnullOwnPtr<ActionObserver> create(WebView::Action& action, QAction& qaction, IncludeActionIcon include_action_icon)
    {
        return adopt_own(*new ActionObserver(action, qaction, include_action_icon));
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

    virtual void on_engaged_state_changed(WebView::Action& action) override
    {
        if (m_include_action_icon == IncludeActionIcon::No)
            return;

        if (!m_action)
            return;

        switch (action.id()) {
        case WebView::ActionID::ToggleBookmark:
        case WebView::ActionID::ToggleBookmarkViaToolbar:
            if (auto* parent = as_if<QWidget>(m_action->parent())) {
                auto icon = action.engaged() ? ChromeIcon::StarFilled : ChromeIcon::Star;
                m_action->setIcon(create_chrome_icon(icon, parent->palette()));
            }
            break;
        default:
            break;
        }
    }

    virtual void on_checked_state_changed(WebView::Action& action) override
    {
        if (m_action)
            m_action->setChecked(action.checked());
    }

private:
    ActionObserver(WebView::Action& action, QAction& qaction, IncludeActionIcon include_action_icon)
        : m_action(&qaction)
        , m_include_action_icon(include_action_icon)
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
    IncludeActionIcon m_include_action_icon { IncludeActionIcon::Yes };
};

template<typename T>
static void add_properties(QObject& object, T& menu_or_action)
{
    for (auto const& [key, value] : menu_or_action.properties())
        object.setProperty(key.to_byte_string().characters(), qstring_from_ak_string(value));
}

static QIcon icon_from_base64_png(StringView favicon_base64_png)
{
    static constexpr int const MENU_ICON_SIZE = 16;

    auto decoded = decode_base64(favicon_base64_png);
    if (decoded.is_error())
        return {};

    QPixmap pixmap;
    if (!pixmap.loadFromData(decoded.value().data(), static_cast<uint>(decoded.value().size()), "PNG"))
        return {};

    QIcon icon;
    for (auto device_pixel_ratio : ICON_DEVICE_PIXEL_RATIOS) {
        auto size = MENU_ICON_SIZE * device_pixel_ratio;
        auto scaled_pixmap = pixmap.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        scaled_pixmap.setDevicePixelRatio(device_pixel_ratio);
        icon.addPixmap(scaled_pixmap);
    }
    return icon;
}

static void initialize_native_control(WebView::Action& action, QAction& qaction, QPalette const& palette, IncludeActionIcon include_action_icon)
{
    switch (action.id()) {
    case WebView::ActionID::NavigateBack:
        if (include_action_icon == IncludeActionIcon::Yes)
            qaction.setIcon(create_chrome_icon(ChromeIcon::Back, palette));
        qaction.setShortcut(QKeySequence::StandardKey::Back);
        break;
    case WebView::ActionID::NavigateForward:
        if (include_action_icon == IncludeActionIcon::Yes)
            qaction.setIcon(create_chrome_icon(ChromeIcon::Forward, palette));
        qaction.setShortcut(QKeySequence::StandardKey::Forward);
        break;
    case WebView::ActionID::Reload:
        if (include_action_icon == IncludeActionIcon::Yes)
            qaction.setIcon(create_chrome_icon(ChromeIcon::Reload, palette));
        qaction.setShortcuts({ QKeySequence(Qt::CTRL | Qt::Key_R), QKeySequence(Qt::Key_F5) });
        break;

    case WebView::ActionID::CopySelection:
        qaction.setShortcut(QKeySequence::StandardKey::Copy);
        break;
    case WebView::ActionID::CutSelection:
        qaction.setShortcut(QKeySequence::StandardKey::Cut);
        break;
    case WebView::ActionID::Paste:
        qaction.setShortcut(QKeySequence::StandardKey::Paste);
        break;
    case WebView::ActionID::SelectAll:
        qaction.setShortcut(QKeySequence::StandardKey::SelectAll);
        break;

    case WebView::ActionID::ToggleBookmark:
        if (include_action_icon == IncludeActionIcon::Yes)
            qaction.setIcon(create_chrome_icon(action.engaged() ? ChromeIcon::StarFilled : ChromeIcon::Star, palette));
        qaction.setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));
        break;
    case WebView::ActionID::ToggleBookmarkViaToolbar:
        if (include_action_icon == IncludeActionIcon::Yes)
            qaction.setIcon(create_chrome_icon(action.engaged() ? ChromeIcon::StarFilled : ChromeIcon::Star, palette));
        break;
    case WebView::ActionID::ToggleBookmarksBar:
        qaction.setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_B));
        break;
    case WebView::ActionID::BookmarkItem:
        if (include_action_icon == IncludeActionIcon::Yes) {
            if (auto icon = action.base64_png_icon(); icon.has_value())
                qaction.setIcon(icon_from_base64_png(*icon));
            else
                qaction.setIcon(create_tvg_icon_with_theme_colors("globe", palette));
        }
        break;

    case WebView::ActionID::OpenProcessesPage:
        qaction.setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M));
        break;
    case WebView::ActionID::OpenSettingsPage:
        qaction.setShortcut(QKeySequence::StandardKey::Preferences);
        break;
    case WebView::ActionID::ToggleDevTools:
        qaction.setShortcuts({
            QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_I),
            QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C),
            QKeySequence(Qt::Key_F12),
        });
        break;
    case WebView::ActionID::ViewSource:
        qaction.setShortcut(QKeySequence(Qt::CTRL | Qt::Key_U));
        break;

    case WebView::ActionID::ZoomIn: {
        auto zoom_in_shortcuts = QKeySequence::keyBindings(QKeySequence::StandardKey::ZoomIn);
        auto secondary_zoom_in_shortcut = QKeySequence(Qt::CTRL | Qt::Key_Equal);

        if (!zoom_in_shortcuts.contains(secondary_zoom_in_shortcut))
            zoom_in_shortcuts.append(move(secondary_zoom_in_shortcut));

        qaction.setShortcuts(zoom_in_shortcuts);
        break;
    }
    case WebView::ActionID::ZoomOut:
        qaction.setShortcut(QKeySequence::StandardKey::ZoomOut);
        break;
    case WebView::ActionID::ResetZoom:
        qaction.setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));
        break;

    default:
        break;
    }

    if (action.is_checkable())
        qaction.setCheckable(true);

    action.add_observer(ActionObserver::create(action, qaction, include_action_icon));
    add_properties(qaction, action);
}

static void add_items_to_menu(QMenu& qmenu, QWidget& parent, WebView::Menu& menu)
{
    add_properties(qmenu, menu);

    for (auto& menu_item : menu.items()) {
        menu_item.visit(
            [&](NonnullRefPtr<WebView::Action>& action) {
                auto* qaction = create_application_action(parent, action, IncludeActionIcon::No);
                qmenu.addAction(qaction);
            },
            [&](NonnullRefPtr<WebView::Menu> const& submenu) {
                auto* qsubmenu = new QMenu(qstring_from_ak_string(submenu->title()), &qmenu);
                add_items_to_menu(*qsubmenu, parent, submenu);

                add_properties(*qsubmenu, *submenu);
                qmenu.addMenu(qsubmenu);
            },
            [&](WebView::Separator) {
                qmenu.addSeparator();
            });
    }
}

QMenu* create_application_menu(QWidget& parent, WebView::Menu& menu)
{
    auto* application_menu = new QMenu(qstring_from_ak_string(menu.title()), &parent);
    add_items_to_menu(*application_menu, parent, menu);
    return application_menu;
}

void repopulate_application_menu(QMenu& menu, QWidget& parent, WebView::Menu& source)
{
    menu.clear();
    add_items_to_menu(menu, parent, source);
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

QAction* create_application_action(QWidget& parent, WebView::Action& action, IncludeActionIcon include_action_icon)
{
    auto* qaction = new QAction(&parent);
    initialize_native_control(action, *qaction, parent.palette(), include_action_icon);
    return qaction;
}

}
