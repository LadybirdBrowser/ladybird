/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibWebView/Application.h>
#include <LibWebView/BookmarkStore.h>
#include <UI/Qt/BookmarksBar.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/Menu.h>
#include <UI/Qt/StringUtils.h>

#include <QAction>
#include <QEvent>
#include <QIcon>
#include <QMenu>
#include <QMouseEvent>
#include <QPointer>
#include <QStyle>
#include <QStyleOptionToolButton>
#include <QStylePainter>
#include <QToolButton>

namespace Ladybird {

static constexpr int BOOKMARK_BUTTON_MAX_WIDTH = 150;
static constexpr int BOOKMARK_BUTTON_ICON_SIZE = 16;
static constexpr int BOOKMARK_BUTTON_MIN_HEIGHT = 24;
static constexpr int BOOKMARK_BUTTON_VERTICAL_PADDING = 8;
static constexpr int BOOKMARK_BUTTON_HORIZONTAL_PADDING = 7;
static constexpr int BOOKMARK_BUTTON_ICON_TEXT_SPACING = 6;
static constexpr int BOOKMARK_BUTTON_TEXT_ELISION_PADDING = 2;

static constexpr char const* BOOKMARK_ITEM_PROPERTY = "bookmark_item";
static constexpr char const* BOOKMARK_CONTEXT_MENU_OPEN_PROPERTY = "bookmark_context_menu_open";

static QStyleOptionToolButton bookmark_button_style_option(QToolButton const& button)
{
    QStyleOptionToolButton option;
    option.initFrom(&button);

    option.rect = button.rect();
    option.icon = button.icon();
    option.iconSize = button.iconSize();
    option.text = button.text();
    option.toolButtonStyle = button.toolButtonStyle();
    option.arrowType = button.arrowType();
    option.subControls = QStyle::SC_ToolButton;

    if (button.arrowType() != Qt::NoArrow)
        option.features |= QStyleOptionToolButton::Arrow;

    switch (button.popupMode()) {
    case QToolButton::DelayedPopup:
        option.features |= QStyleOptionToolButton::PopupDelay;
        break;
    case QToolButton::MenuButtonPopup:
        option.features |= QStyleOptionToolButton::MenuButtonPopup;
        option.subControls |= QStyle::SC_ToolButtonMenu;
        break;
    case QToolButton::InstantPopup:
        option.features |= QStyleOptionToolButton::Menu;
        break;
    }

    if (button.autoRaise())
        option.state |= QStyle::State_AutoRaise;
    if (button.menu())
        option.features |= QStyleOptionToolButton::HasMenu;
    if (button.isDown())
        option.state |= QStyle::State_Sunken;
    if (button.property(BOOKMARK_CONTEXT_MENU_OPEN_PROPERTY).toBool())
        option.state |= QStyle::State_MouseOver;

    return option;
}

struct BookmarkButtonLayout {
    QRect content_rect;
    QRect icon_rect;
    QRect text_rect;
    int menu_indicator_width { 0 };
    int icon_width { 0 };
    int icon_text_spacing { 0 };
    int available_text_width { 0 };
    int preferred_width { 0 };
};
static BookmarkButtonLayout bookmark_button_layout(QToolButton const& button, QStyleOptionToolButton const& option, QString const& text, QRect const& button_rect)
{
    BookmarkButtonLayout layout;

    if (button.menu())
        layout.menu_indicator_width = button.style()->pixelMetric(QStyle::PM_MenuButtonIndicator, &option, &button);

    layout.icon_width = button.icon().isNull() ? 0 : button.iconSize().width();
    layout.icon_text_spacing = layout.icon_width > 0 && !text.isEmpty() ? BOOKMARK_BUTTON_ICON_TEXT_SPACING : 0;

    layout.content_rect = button_rect;
    layout.content_rect.adjust(BOOKMARK_BUTTON_HORIZONTAL_PADDING, 0, -BOOKMARK_BUTTON_HORIZONTAL_PADDING, 0);
    layout.content_rect.adjust(0, 0, -layout.menu_indicator_width, 0);

    auto text_left = layout.content_rect.left() + layout.icon_width + layout.icon_text_spacing;
    layout.available_text_width = max(layout.content_rect.right() - text_left + 1, 0);
    layout.text_rect = QRect { text_left, layout.content_rect.top(), layout.available_text_width, layout.content_rect.height() };

    if (layout.icon_width > 0) {
        auto icon_size = option.iconSize;
        layout.icon_rect = QRect {
            layout.content_rect.left(),
            layout.content_rect.top() + ((layout.content_rect.height() - icon_size.height()) / 2),
            icon_size.width(),
            icon_size.height(),
        };
    }

    auto preferred_width = (BOOKMARK_BUTTON_HORIZONTAL_PADDING * 2)
        + layout.icon_width
        + layout.icon_text_spacing
        + button.fontMetrics().horizontalAdvance(text)
        + BOOKMARK_BUTTON_TEXT_ELISION_PADDING
        + layout.menu_indicator_width;
    layout.preferred_width = min(preferred_width, button_rect.width());

    return layout;
}

static void paint_bookmark_button(QToolButton& button)
{
    QStylePainter painter(&button);

    auto option = bookmark_button_style_option(button);
    auto layout = bookmark_button_layout(button, option, option.text, button.rect());

    auto frame_option = option;
    frame_option.icon = {};
    frame_option.text.clear();
    painter.drawComplexControl(QStyle::CC_ToolButton, frame_option);

    if (layout.icon_width > 0) {
        auto mode = button.isEnabled() ? QIcon::Normal : QIcon::Disabled;
        if (button.isEnabled() && (option.state & QStyle::State_MouseOver))
            mode = QIcon::Active;

        option.icon.paint(&painter, layout.icon_rect, Qt::AlignCenter, mode, button.isChecked() ? QIcon::On : QIcon::Off);
    }

    auto elided_text = button.fontMetrics().elidedText(option.text, Qt::ElideRight, layout.available_text_width);

    button.style()->drawItemText(
        &painter,
        layout.text_rect,
        Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
        option.palette,
        button.isEnabled(),
        elided_text,
        QPalette::ButtonText);
}

static void install_menu_event_filter(QObject* filter, QMenu* menu)
{
    menu->installEventFilter(filter);

    for (auto* action : menu->actions()) {
        if (auto* submenu = action->menu())
            install_menu_event_filter(filter, submenu);
    }
}

BookmarksBar::BookmarksBar(QWidget* parent)
    : QToolBar(parent)
{
    setObjectName("LadybirdBookmarksBar");
    setIconSize({ BOOKMARK_BUTTON_ICON_SIZE, BOOKMARK_BUTTON_ICON_SIZE });
    setVisible(WebView::Application::settings().show_bookmarks_bar());
    setMovable(false);
    setFloatable(false);
    update_chrome_style();

    installEventFilter(this);

    rebuild();
}

bool BookmarksBar::event(QEvent* event)
{
    if (event->type() == QEvent::PaletteChange)
        update_chrome_style();

    return QToolBar::event(event);
}

void BookmarksBar::update_chrome_style()
{
    if (m_is_updating_chrome_style)
        return;

    m_is_updating_chrome_style = true;
    setStyleSheet(ChromeStyle::bookmarks_bar_style_sheet(palette()));
    m_is_updating_chrome_style = false;
}

void BookmarksBar::rebuild()
{
    for (auto* action : actions()) {
        if (auto* menu = action->menu())
            menu->close();
    }

    clear();

    auto set_button_properties = [&](QToolButton* button, QString const& title) {
        button->setProperty(BOOKMARK_ITEM_PROPERTY, true);
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->ensurePolished();

        auto option = bookmark_button_style_option(*button);

        auto bookmark_button_height = max(BOOKMARK_BUTTON_MIN_HEIGHT, max(button->fontMetrics().height(), button->iconSize().height()) + BOOKMARK_BUTTON_VERTICAL_PADDING);
        auto max_size_rect = QRect { 0, 0, BOOKMARK_BUTTON_MAX_WIDTH, bookmark_button_height };

        auto layout = bookmark_button_layout(*button, option, title, max_size_rect);

        auto available_title_width = max(layout.available_text_width - BOOKMARK_BUTTON_TEXT_ELISION_PADDING, 0);
        auto text = button->fontMetrics().elidedText(title, Qt::ElideRight, available_title_width);
        button->setText(text);

        layout = bookmark_button_layout(*button, option, text, max_size_rect);

        button->setFixedWidth(layout.preferred_width);
        button->setMaximumWidth(BOOKMARK_BUTTON_MAX_WIDTH);
        button->setFixedHeight(bookmark_button_height);

        button->installEventFilter(this);
    };

    for (auto const& item : WebView::Application::the().bookmarks_menu().items()) {
        item.visit(
            [&](NonnullRefPtr<WebView::Action> const& bookmark) {
                if (bookmark->id() != WebView::ActionID::BookmarkItem)
                    return;

                auto* action = create_application_action(*this, *bookmark);
                addAction(action);

                if (auto* button = as_if<QToolButton>(widgetForAction(action)))
                    set_button_properties(button, qstring_from_ak_string(bookmark->text()));
            },
            [&](NonnullRefPtr<WebView::Menu> const& folder) {
                auto title = qstring_from_ak_string(folder->title());

                auto* submenu = create_application_menu(*this, *folder);
                install_menu_event_filter(this, submenu);

                auto* action = new QAction(title, this);
                action->setIcon(create_chrome_icon(ChromeIcon::Folder, palette()));
                action->setProperty("id", submenu->property("id"));
                action->setProperty("type", submenu->property("type"));
                action->setProperty("target_folder_id", submenu->property("target_folder_id"));
                action->setMenu(submenu);
                addAction(action);

                if (auto* button = as_if<QToolButton>(widgetForAction(action))) {
                    button->setPopupMode(QToolButton::InstantPopup);
                    set_button_properties(button, title);
                }
            },
            [](WebView::Separator) {
            });
    }
}

void BookmarksBar::show_context_menu(QPoint position, Optional<WebView::BookmarkItem const&> item, Optional<String const&> target_folder_id)
{
    if (item.has_value()) {
        m_selected_bookmark_menu_item_id = item->id;
        m_selected_bookmark_menu_target_folder_id = target_folder_id.copy();

        if (item->is_bookmark())
            bookmark_context_menu().exec(position);
        else if (item->is_folder())
            bookmark_folder_context_menu().exec(position);
    } else {
        m_selected_bookmark_menu_item_id = {};
        m_selected_bookmark_menu_target_folder_id = {};

        bookmarks_bar_context_menu().exec(position);
    }
}

bool BookmarksBar::eventFilter(QObject* object, QEvent* event)
{
    if (event->type() == QEvent::Paint) {
        if (auto* button = as_if<QToolButton>(object); button && button->property(BOOKMARK_ITEM_PROPERTY).toBool()) {
            paint_bookmark_button(*button);
            return true;
        }
    }

    if (event->type() == QEvent::MouseButtonPress) {
        auto& mouse_event = as<QMouseEvent>(*event);

        if (mouse_event.button() == Qt::LeftButton)
            return handle_left_mouse_click(&mouse_event, object);
        if (mouse_event.button() == Qt::MiddleButton)
            return handle_middle_mouse_click(&mouse_event, object);
        if (mouse_event.button() == Qt::RightButton)
            return handle_right_mouse_click(&mouse_event, object);
    }

    return QToolBar::eventFilter(object, event);
}

bool BookmarksBar::handle_left_mouse_click(QMouseEvent* event, QObject* item)
{
    if (event->modifiers().testFlag(Qt::ControlModifier))
        return handle_middle_mouse_click(event, item);
    return false;
}

bool BookmarksBar::handle_middle_mouse_click(QMouseEvent* event, QObject* item)
{
    auto activate_tab = event->modifiers().testFlag(Qt::ShiftModifier) ? Web::HTML::ActivateTab::No : Web::HTML::ActivateTab::Yes;

    if (auto* button = as_if<QToolButton>(item)) {
        auto* action = button->defaultAction();
        extract_item_properties(action);

        if (m_selected_bookmark_menu_item_type == "bookmark")
            WebView::Application::the().open_bookmark_in_new_tab(m_selected_bookmark_menu_item_id, activate_tab);
    } else if (auto* menu = as_if<QMenu>(item)) {
        if (auto* action = menu->actionAt(event->pos())) {
            extract_item_properties(action);

            if (m_selected_bookmark_menu_item_type == "bookmark")
                WebView::Application::the().open_bookmark_in_new_tab(m_selected_bookmark_menu_item_id, activate_tab);
        }
    }

    return true;
}

bool BookmarksBar::handle_right_mouse_click(QMouseEvent* event, QObject* item)
{
    if (is<BookmarksBar>(item)) {
        m_selected_bookmark_menu_item_id = {};
        m_selected_bookmark_menu_target_folder_id = {};

        bookmarks_bar_context_menu().exec(event->globalPosition().toPoint());
    } else if (auto* button = as_if<QToolButton>(item)) {
        auto* action = button->defaultAction();
        extract_item_properties(action);

        auto set_button_context_menu_property = [button = QPointer { button }](bool open) {
            if (button) {
                button->setProperty(BOOKMARK_CONTEXT_MENU_OPEN_PROPERTY, open);
                button->update();
            }
        };

        ScopeGuard guard { [&]() { set_button_context_menu_property(false); } };
        set_button_context_menu_property(true);

        if (m_selected_bookmark_menu_item_type == "bookmark")
            bookmark_context_menu().exec(event->globalPosition().toPoint());
        else if (m_selected_bookmark_menu_item_type == "folder")
            bookmark_folder_context_menu().exec(event->globalPosition().toPoint());
    } else if (auto* menu = as_if<QMenu>(item)) {
        if (auto* action = menu->actionAt(event->pos())) {
            QObject* submenu = action->menu();
            extract_item_properties(submenu ?: action);
        }

        if (m_selected_bookmark_menu_item_type.isEmpty())
            extract_item_properties(menu);

        // FIXME: We create a temporary context menu parented to the dropdown. Otherwise, Qt complains that the context
        //        menu's parent does not match the current topmost popup. It would be nice if we could figure out a way
        //        to avoid this duplicated menu.
        QMenu context_menu(menu);

        if (m_selected_bookmark_menu_item_type == "bookmark")
            repopulate_application_menu(context_menu, context_menu, WebView::Application::the().bookmark_context_menu());
        else if (m_selected_bookmark_menu_item_type == "folder")
            repopulate_application_menu(context_menu, context_menu, WebView::Application::the().bookmark_folder_context_menu());

        if (!context_menu.isEmpty() && context_menu.exec(event->globalPosition().toPoint()))
            menu->close();
    }

    return true;
}

void BookmarksBar::extract_item_properties(QObject* item)
{
    m_selected_bookmark_menu_item_id = ak_string_from_qstring(item->property("id").toString());
    m_selected_bookmark_menu_item_type = item->property("type").toString();

    if (auto value = ak_string_from_qstring(item->property("target_folder_id").toString()); !value.is_empty())
        m_selected_bookmark_menu_target_folder_id = AK::move(value);
}

QMenu& BookmarksBar::bookmarks_bar_context_menu()
{
    if (!m_bookmarks_bar_context_menu)
        m_bookmarks_bar_context_menu = create_application_menu(*this, WebView::Application::the().bookmarks_bar_context_menu());
    return *m_bookmarks_bar_context_menu;
}

QMenu& BookmarksBar::bookmark_context_menu()
{
    if (!m_bookmark_context_menu)
        m_bookmark_context_menu = create_application_menu(*this, WebView::Application::the().bookmark_context_menu());
    return *m_bookmark_context_menu;
}

QMenu& BookmarksBar::bookmark_folder_context_menu()
{
    if (!m_bookmark_folder_context_menu)
        m_bookmark_folder_context_menu = create_application_menu(*this, WebView::Application::the().bookmark_folder_context_menu());
    return *m_bookmark_folder_context_menu;
}

}
