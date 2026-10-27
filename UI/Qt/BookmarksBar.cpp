/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/ScopeGuard.h>
#include <LibWebView/Application.h>
#include <LibWebView/BookmarkStore.h>
#include <UI/Qt/BookmarksBar.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/Menu.h>
#include <UI/Qt/Motion.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/SystemAppearance.h>
#include <UI/Qt/Tab.h>
#include <UI/Qt/Theme.h>

#include <QAction>
#include <QEvent>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPointer>
#include <QStyle>
#include <QStyleOptionToolButton>
#include <QStylePainter>
#include <QTimer>
#include <QToolButton>
#include <QVariantAnimation>

namespace Ladybird {

// Bookmarks are 24 px buttons with 6 px corners, their favicon padded less than their title.
static constexpr int BOOKMARK_BUTTON_MAX_WIDTH = 168;
static constexpr int BOOKMARK_BUTTON_ICON_SIZE = 14;
static constexpr int BOOKMARK_BUTTON_HEIGHT = 24;
static constexpr int BOOKMARK_BUTTON_CORNER_RADIUS = 6;
static constexpr int BOOKMARK_BUTTON_LEFT_PADDING = 6;
static constexpr int BOOKMARK_BUTTON_RIGHT_PADDING = 8;
static constexpr int BOOKMARK_TITLE_PIXEL_SIZE = 12;
static constexpr int BOOKMARK_BUTTON_ICON_TEXT_SPACING = 6;
static constexpr int BOOKMARK_BUTTON_TEXT_ELISION_PADDING = 2;

static constexpr char const* BOOKMARK_ITEM_PROPERTY = "bookmark_item";
static constexpr char const* BOOKMARK_CONTEXT_MENU_OPEN_PROPERTY = "bookmark_context_menu_open";
static constexpr char const* BOOKMARK_FOLDER_MENU_OPEN_PROPERTY = "bookmark_folder_menu_open";

static QFont bookmark_title_font(QToolButton const& button)
{
    auto font = ChromeStyle::chrome_title_font(button.font());
    font.setPixelSize(BOOKMARK_TITLE_PIXEL_SIZE);
    return font;
}

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
    // NB: A folder's button stays pressed while its menu is open, also when the menu was opened by moving to it.
    if (button.isDown() || button.property(BOOKMARK_FOLDER_MENU_OPEN_PROPERTY).toBool())
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
    layout.content_rect.adjust(BOOKMARK_BUTTON_LEFT_PADDING, 0, -BOOKMARK_BUTTON_RIGHT_PADDING, 0);
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

    auto preferred_width = BOOKMARK_BUTTON_LEFT_PADDING + BOOKMARK_BUTTON_RIGHT_PADDING
        + layout.icon_width
        + layout.icon_text_spacing
        + QFontMetrics(bookmark_title_font(button)).horizontalAdvance(text)
        + BOOKMARK_BUTTON_TEXT_ELISION_PADDING
        + layout.menu_indicator_width;
    layout.preferred_width = min(preferred_width, button_rect.width());

    return layout;
}

static void paint_bookmark_button(QToolButton& button)
{
    QStylePainter painter(&button);

    auto option = bookmark_button_style_option(button);
    // While it slides in, a button is laid out at its full width and cut off by its edge, rather than squeezed.
    auto layout_rect = button.rect();
    if (auto full_width = button.property("slidingInWidth").toInt(); full_width > 0)
        layout_rect.setWidth(full_width);
    auto layout = bookmark_button_layout(button, option, option.text, layout_rect);

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

    auto title_font = bookmark_title_font(button);
    auto elided_text = QFontMetrics(title_font).elidedText(option.text, Qt::ElideRight, layout.available_text_width);

    // NB: The style would draw the title in the button's own font, so draw it here.
    painter.setFont(title_font);
    painter.setPen(option.palette.color(button.isEnabled() ? QPalette::Active : QPalette::Disabled, QPalette::ButtonText));
    painter.drawText(layout.text_rect, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, elided_text);
}

static void install_menu_event_filter(QObject* filter, QMenu* menu)
{
    menu->installEventFilter(filter);

    for (auto* action : menu->actions()) {
        if (auto* submenu = action->menu())
            install_menu_event_filter(filter, submenu);
    }
}

BookmarksBar::BookmarksBar(Tab* parent)
    : QToolBar(parent)
    , m_tab(parent)
{
    setObjectName("LadybirdBookmarksBar");
    setIconSize({ BOOKMARK_BUTTON_ICON_SIZE, BOOKMARK_BUTTON_ICON_SIZE });
    setVisible(false);
    setMovable(false);
    setFloatable(false);
    update_chrome_style();

    installEventFilter(this);

    rebuild();
}

void BookmarksBar::update_chrome_style()
{
    setStyleSheet(ChromeStyle::bookmarks_bar_style_sheet(Theme::chrome_palette(*m_tab), BOOKMARK_BUTTON_CORNER_RADIUS));
}

// A bookmark that is added slides into the bar, and the items after it move to make room. One that is removed slides
// out of it, and they close up after it.
void BookmarksBar::slide_in(QWidget& widget, int width)
{
    auto* animation = new QVariantAnimation(&widget);
    animation->setDuration(Motion::BASE);
    animation->setEasingCurve(Motion::move_easing());
    animation->setStartValue(0);
    animation->setEndValue(width);
    connect(animation, &QVariantAnimation::valueChanged, &widget, [&widget](QVariant const& value) {
        widget.setFixedWidth(value.toInt());
    });
    connect(animation, &QVariantAnimation::finished, &widget, [&widget] {
        widget.setProperty("slidingInWidth", {});
        widget.update();
    });
    widget.setProperty("slidingInWidth", width);
    widget.setFixedWidth(0);
    animation->start(QAbstractAnimation::DeleteWhenStopped);
}

void BookmarksBar::slide_out(int index, QPixmap const& image)
{
    auto items = actions();
    // The picture of the item is cut off by the edge of the room it leaves, as that closes.
    auto* placeholder = new QLabel(this);
    placeholder->setPixmap(image);
    placeholder->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    auto width = qRound(image.width() / image.devicePixelRatio());
    placeholder->setFixedSize(width, BOOKMARK_BUTTON_HEIGHT);
    auto* placeholder_action = index < items.size() ? insertWidget(items[index], placeholder) : addWidget(placeholder);

    auto* animation = new QVariantAnimation(placeholder);
    animation->setDuration(Motion::BASE);
    animation->setEasingCurve(Motion::move_easing());
    animation->setStartValue(width);
    animation->setEndValue(0);
    connect(animation, &QVariantAnimation::valueChanged, placeholder, [placeholder](QVariant const& value) {
        placeholder->setFixedWidth(value.toInt());
    });
    connect(animation, &QVariantAnimation::finished, this, [this, placeholder_action = QPointer { placeholder_action }] {
        if (placeholder_action) {
            removeAction(placeholder_action);
            placeholder_action->deleteLater();
        }
    });
    animation->start(QAbstractAnimation::DeleteWhenStopped);
}

void BookmarksBar::rebuild()
{
    for (auto* action : actions()) {
        if (auto* menu = action->menu())
            menu->close();
    }

    // The items the bar showed before, so those added or removed since can slide in or out.
    auto animates = m_has_been_built && !SystemAppearance::prefers_reduced_motion();
    struct PreviousItem {
        int index { 0 };
        // A picture of the item, which slides out of the bar if the item is gone.
        QPixmap image;
    };
    HashMap<String, PreviousItem> previous_items;
    {
        int index = 0;
        for (auto* action : actions()) {
            auto id = action->property("id").toString();
            if (id.isEmpty())
                continue;
            auto* widget = widgetForAction(action);
            previous_items.set(ak_string_from_qstring(id), { index++, widget && animates ? widget->grab() : QPixmap {} });
        }
    }
    m_has_been_built = true;
    HashTable<String> current_ids;

    clear();

    auto set_button_properties = [&](QToolButton* button, QString const& title) {
        button->setProperty(BOOKMARK_ITEM_PROPERTY, true);
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->ensurePolished();

        auto option = bookmark_button_style_option(*button);

        QFontMetrics title_font_metrics(bookmark_title_font(*button));
        auto height = BOOKMARK_BUTTON_HEIGHT;
        auto max_size_rect = QRect { 0, 0, BOOKMARK_BUTTON_MAX_WIDTH, height };

        auto layout = bookmark_button_layout(*button, option, title, max_size_rect);

        auto available_title_width = max(layout.available_text_width - BOOKMARK_BUTTON_TEXT_ELISION_PADDING, 0);
        auto text = title_font_metrics.elidedText(title, Qt::ElideRight, available_title_width);
        button->setText(text);

        layout = bookmark_button_layout(*button, option, text, max_size_rect);

        button->setFixedWidth(layout.preferred_width);
        button->setMaximumWidth(BOOKMARK_BUTTON_MAX_WIDTH);
        button->setFixedHeight(height);

        button->installEventFilter(this);

        auto id = ak_string_from_qstring(button->defaultAction() ? button->defaultAction()->property("id").toString() : QString {});
        current_ids.set(id);
        if (animates && !id.is_empty() && !previous_items.contains(id))
            slide_in(*button, layout.preferred_width);
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
                if (folder->properties().get("type"sv) != "folder"sv)
                    return;

                auto title = qstring_from_ak_string(folder->title());

                auto* submenu = create_application_menu(*this, *folder);
                install_menu_event_filter(this, submenu);

                auto* action = new QAction(title, this);
                action->setIcon(create_chrome_icon(ChromeIcon::Folder, palette()));
                action->setProperty("id", submenu->property("id"));
                action->setProperty("type", submenu->property("type"));
                action->setProperty("target_folder_id", submenu->property("target_folder_id"));
                action->setProperty("parent_folder_id", submenu->property("parent_folder_id"));
                action->setMenu(submenu);
                addAction(action);

                if (auto* button = as_if<QToolButton>(widgetForAction(action))) {
                    button->setPopupMode(QToolButton::InstantPopup);
                    set_button_properties(button, title);
                    auto set_menu_open = [button = QPointer { button }](bool open) {
                        if (!button)
                            return;
                        button->setProperty(BOOKMARK_FOLDER_MENU_OPEN_PROPERTY, open);
                        button->update();
                    };
                    QObject::connect(submenu, &QMenu::aboutToShow, button, [set_menu_open] { set_menu_open(true); });
                    QObject::connect(submenu, &QMenu::aboutToHide, button, [set_menu_open] { set_menu_open(false); });
                }
            },
            [](WebView::Separator) {
            });
    }

    if (animates) {
        for (auto const& [id, item] : previous_items) {
            if (!current_ids.contains(id) && !item.image.isNull())
                slide_out(item.index, item.image);
        }
    }
}

void BookmarksBar::show_context_menu(QPoint position, Optional<WebView::BookmarkItem const&> item, Optional<String const&> target_folder_id, Optional<String const&> parent_folder_id)
{
    if (item.has_value()) {
        m_selected_bookmark_menu_item_id = item->id;
        m_selected_bookmark_menu_target_folder_id = target_folder_id.copy();
        m_selected_bookmark_menu_parent_folder_id = parent_folder_id.copy();

        if (item->is_bookmark())
            execute_context_menu(bookmark_context_menu(), position);
        else if (item->is_folder())
            execute_context_menu(bookmark_folder_context_menu(), position);
    } else {
        // NB: Empty space in a folder that about:bookmarks shows adds to that folder, and elsewhere to the bar.
        m_selected_bookmark_menu_item_id = {};
        m_selected_bookmark_menu_target_folder_id = target_folder_id.copy();
        m_selected_bookmark_menu_parent_folder_id = {};

        execute_context_menu(bookmarks_bar_context_menu(), position);
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

    // The folders on the bar behave like a menu bar: while one is open, moving over another opens that one instead.
    if (event->type() == QEvent::MouseMove) {
        if (auto* menu = as_if<QMenu>(object)) {
            auto global_position = as<QMouseEvent>(*event).globalPosition().toPoint();
            auto* button = as_if<QToolButton>(childAt(mapFromGlobal(global_position)));
            // NB: A folder's menu is its button's action's.
            auto* folder_menu = button && button->defaultAction() ? button->defaultAction()->menu() : nullptr;
            if (folder_menu && !folder_menu->isVisible()) {
                for (auto* open_menu = menu; open_menu; open_menu = as_if<QMenu>(open_menu->parentWidget()))
                    open_menu->close();
                QTimer::singleShot(0, button, [button = QPointer { button }] {
                    if (button)
                        button->showMenu();
                });
                return true;
            }
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
    // FIXME: The exec() calls below should use execute_context_menu() instead to use native context menus on macOS.
    //        However, more work is needed to ensure that opened bookmark folders do not disappear when the native menu
    //        is shown.

    if (is<BookmarksBar>(item)) {
        m_selected_bookmark_menu_item_id = {};
        m_selected_bookmark_menu_target_folder_id = {};
        m_selected_bookmark_menu_parent_folder_id = {};

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
            repopulate_application_menu(context_menu, context_menu, m_tab->view().bookmark_context_menu());
        else if (m_selected_bookmark_menu_item_type == "folder")
            repopulate_application_menu(context_menu, context_menu, m_tab->view().bookmark_folder_context_menu());

        if (!context_menu.isEmpty() && context_menu.exec(event->globalPosition().toPoint()))
            menu->close();
    }

    return true;
}

void BookmarksBar::extract_item_properties(QObject* item)
{
    m_selected_bookmark_menu_item_id = ak_string_from_qstring(item->property("id").toString());
    m_selected_bookmark_menu_item_type = item->property("type").toString();

    m_selected_bookmark_menu_target_folder_id = {};
    if (auto value = ak_string_from_qstring(item->property("target_folder_id").toString()); !value.is_empty())
        m_selected_bookmark_menu_target_folder_id = AK::move(value);

    m_selected_bookmark_menu_parent_folder_id = {};
    if (auto value = ak_string_from_qstring(item->property("parent_folder_id").toString()); !value.is_empty())
        m_selected_bookmark_menu_parent_folder_id = AK::move(value);
}

QMenu& BookmarksBar::bookmarks_bar_context_menu()
{
    if (!m_bookmarks_bar_context_menu)
        m_bookmarks_bar_context_menu = create_application_menu(*this, m_tab->view().bookmarks_bar_context_menu());
    return *m_bookmarks_bar_context_menu;
}

QMenu& BookmarksBar::bookmark_context_menu()
{
    if (!m_bookmark_context_menu)
        m_bookmark_context_menu = create_application_menu(*this, m_tab->view().bookmark_context_menu());
    return *m_bookmark_context_menu;
}

QMenu& BookmarksBar::bookmark_folder_context_menu()
{
    if (!m_bookmark_folder_context_menu)
        m_bookmark_folder_context_menu = create_application_menu(*this, m_tab->view().bookmark_folder_context_menu());
    return *m_bookmark_folder_context_menu;
}

}
