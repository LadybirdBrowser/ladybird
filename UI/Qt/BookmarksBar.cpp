/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <LibWebView/Application.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/BookmarkStore.h>
#include <UI/Qt/Bookmarks.h>
#include <UI/Qt/BookmarksBar.h>
#include <UI/Qt/BrowserWindow.h>
#include <UI/Qt/ChromeLayout.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/HubConnection.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/Menu.h>
#include <UI/Qt/Motion.h>
#include <UI/Qt/Popover.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/SystemAppearance.h>
#include <UI/Qt/Tab.h>
#include <UI/Qt/Theme.h>
#include <UI/Qt/WindowToolbar.h>
#if defined(AK_OS_MACOS)
#    include <UI/Qt/MacWindow.h>
#endif

#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QDynamicPropertyChangeEvent>
#include <QEvent>
#include <QFontMetricsF>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QStyle>
#include <QStyleOptionToolButton>
#include <QStylePainter>
#include <QTimer>
#include <QToolButton>
#include <QVariantAnimation>
#include <QWheelEvent>
#include <QtMath>

namespace Ladybird {

// Bookmarks are 24 px buttons with 6 px corners, padded as much after their title as before their favicon.
static constexpr int BOOKMARK_BUTTON_MAX_WIDTH = 168;
static constexpr int BOOKMARK_BUTTON_ICON_SIZE = 16;
static constexpr int BOOKMARK_BUTTON_HEIGHT = 24;
static constexpr int BOOKMARK_BUTTON_CORNER_RADIUS = 6;
static constexpr int BOOKMARK_BUTTON_HORIZONTAL_PADDING = 6;
static constexpr int BOOKMARK_TITLE_PIXEL_SIZE = 12;
static constexpr int BOOKMARK_BUTTON_ICON_TEXT_SPACING = 6;

static constexpr char const* BOOKMARK_ITEM_PROPERTY = "bookmark_item";
static constexpr char const* BOOKMARK_CONTEXT_MENU_OPEN_PROPERTY = "bookmark_context_menu_open";
static constexpr char const* BOOKMARK_FOLDER_MENU_OPEN_PROPERTY = "bookmark_folder_menu_open";
static constexpr char const* BOOKMARK_DROP_TARGET_PROPERTY = "bookmark_drop_target";

static constexpr int BOOKMARK_DROP_INDICATOR_WIDTH = 2;
static constexpr qreal BOOKMARK_DRAG_IMAGE_OPACITY = 0.7;
static constexpr int BOOKMARK_DRAG_MENU_SCROLL_INTERVAL_MS = 50;

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
    // While a bookmark is dragged, only a folder it can go into looks hovered.
    if (auto const* bar = as_if<BookmarksBar>(button.parentWidget()); bar && bar->is_dragging_bookmark())
        option.state &= ~QStyle::State_MouseOver;
    if (button.property(BOOKMARK_CONTEXT_MENU_OPEN_PROPERTY).toBool() || button.property(BOOKMARK_DROP_TARGET_PROPERTY).toBool())
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

    auto preferred_width = BOOKMARK_BUTTON_HORIZONTAL_PADDING * 2
        + layout.icon_width
        + layout.icon_text_spacing
        + qCeil(QFontMetricsF(bookmark_title_font(button)).horizontalAdvance(text))
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

static void show_menu_later(QToolButton& button)
{
    QTimer::singleShot(0, &button, &QToolButton::showMenu);
}

static QMenu* parent_menu(QMenu const& menu)
{
    if (auto* parent = as_if<QMenu>(menu.parentWidget()))
        return parent;
    // The menu of a folder that does not fit on the bar opens from the overflow menu.
    auto const* bar = as_if<BookmarksBar>(menu.parentWidget());
    auto* overflow_menu = bar ? bar->overflow_menu() : nullptr;
    if (overflow_menu && overflow_menu->actions().contains(menu.menuAction()))
        return overflow_menu;
    return nullptr;
}

static void close_menu_chain(QMenu& menu)
{
    for (auto* open_menu = &menu; open_menu; open_menu = parent_menu(*open_menu))
        open_menu->close();
}

static bool is_menu_or_parent_of(QMenu const& menu, QMenu const* other)
{
    for (auto const* ancestor = other; ancestor; ancestor = parent_menu(*ancestor)) {
        if (ancestor == &menu)
            return true;
    }
    return false;
}

static QMenu& innermost_open_menu(QMenu& menu)
{
    auto* innermost_menu = &menu;
    while (auto* action = innermost_menu->activeAction()) {
        auto* submenu = action->menu();
        if (!submenu || !submenu->isVisible())
            break;
        innermost_menu = submenu;
    }
    return *innermost_menu;
}

static void activate_entries_leading_to(QMenu& menu)
{
    for (auto* submenu = &menu; auto* parent = parent_menu(*submenu); submenu = parent) {
        // Activating an entry centers it in a menu that scrolls, even when it is already active.
        if (parent->activeAction() != submenu->menuAction())
            parent->setActiveAction(submenu->menuAction());
    }
}

static void close_open_submenu(QMenu& menu, QMenu const* submenu_to_keep)
{
    auto* active_action = menu.activeAction();
    auto* submenu = active_action ? active_action->menu() : nullptr;
    if (!submenu || submenu == submenu_to_keep || !submenu->isVisible())
        return;
    for (auto* open_menu = &innermost_open_menu(*submenu); open_menu && open_menu != &menu; open_menu = parent_menu(*open_menu))
        open_menu->close();
}

static QRect menu_content_rect(QMenu const& menu)
{
    auto const* style = menu.style();
    auto inset = style->pixelMetric(QStyle::PM_MenuPanelWidth, nullptr, &menu) + style->pixelMetric(QStyle::PM_MenuVMargin, nullptr, &menu);
    return menu.rect().marginsRemoved(menu.contentsMargins()).adjusted(0, inset, 0, -inset);
}

struct MenuOverflow {
    bool above { false };
    bool below { false };
};

static MenuOverflow menu_overflow(QMenu const& menu)
{
    auto content_rect = menu_content_rect(menu);
    MenuOverflow overflow;
    for (auto* action : menu.actions()) {
        auto geometry = menu.actionGeometry(action);
        if (geometry.isNull())
            continue;
        if (geometry.top() < content_rect.top())
            overflow.above = true;
        if (geometry.bottom() > content_rect.bottom())
            overflow.below = true;
    }
    return overflow;
}

static bool menu_scrolls(QMenu const& menu)
{
    auto overflow = menu_overflow(menu);
    return overflow.above || overflow.below;
}

enum class MenuScrollArrow : u8 {
    None,
    Up,
    Down,
};

static MenuScrollArrow menu_scroll_arrow_at(QMenu const& menu, QPoint position)
{
    if (position.x() < 0 || position.x() >= menu.width())
        return MenuScrollArrow::None;

    auto content_rect = menu_content_rect(menu);
    auto arrow_height = menu.style()->pixelMetric(QStyle::PM_MenuScrollerHeight, nullptr, &menu);
    auto overflow = menu_overflow(menu);
    if (overflow.above && position.y() <= content_rect.top() + arrow_height)
        return MenuScrollArrow::Up;
    if (overflow.below && position.y() > content_rect.bottom() - arrow_height)
        return MenuScrollArrow::Down;
    return MenuScrollArrow::None;
}

static QString bookmark_item_id(QAction const& action)
{
    QObject const* entry = action.menu() ? static_cast<QObject const*>(action.menu()) : &action;
    return entry->property("id").toString();
}

BookmarksBar::BookmarksBar(QWidget* parent)
    : QToolBar(parent)
{
    setObjectName("LadybirdBookmarksBar");
    setIconSize({ BOOKMARK_BUTTON_ICON_SIZE, BOOKMARK_BUTTON_ICON_SIZE });
    setVisible(false);
    setMovable(false);
    setFloatable(false);
    setProperty(WINDOW_DRAG_REGION_PROPERTY, true);
    setContextMenuPolicy(Qt::PreventContextMenu);

    installEventFilter(this);

    m_overflow_button = findChild<QToolButton*>(QStringLiteral("qt_toolbar_ext_button"), Qt::FindDirectChildrenOnly);
    update_chrome_style();
    if (auto* menu = overflow_menu()) {
        menu->installEventFilter(this);
        connect(menu, &QMenu::aboutToShow, m_overflow_button.data(), [this] { m_overflow_button->setDown(true); });
        connect(menu, &QMenu::aboutToHide, m_overflow_button.data(), [this] { m_overflow_button->setDown(false); });
    }

    m_drag_menu_scroll_timer = new QTimer(this);
    m_drag_menu_scroll_timer->setInterval(BOOKMARK_DRAG_MENU_SCROLL_INTERVAL_MS);
    connect(m_drag_menu_scroll_timer, &QTimer::timeout, this, [this] { scroll_drag_menu(); });

    rebuild();
}

QMenu* BookmarksBar::overflow_menu() const
{
    return m_overflow_button ? m_overflow_button->menu() : nullptr;
}

size_t BookmarksBar::first_overflow_index() const
{
    auto const* menu = overflow_menu();
    size_t index = 0;
    for (auto* action : actions()) {
        if (bookmark_item_id(*action).isEmpty())
            continue;
        if (menu && menu->actions().contains(action))
            break;
        ++index;
    }
    return index;
}

BookmarksBar::~BookmarksBar()
{
    stop_drag();
}

void BookmarksBar::update_chrome_style()
{
    auto palette = Theme::chrome_palette(*this);
    setStyleSheet(ChromeStyle::bookmarks_bar_style_sheet(palette, BOOKMARK_BUTTON_CORNER_RADIUS));
    if (m_overflow_button)
        m_overflow_button->setIcon(create_chrome_icon(ChromeIcon::DoubleChevronRight, palette));
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
    stop_drag();

    for (auto* action : actions()) {
        if (auto* menu = action->menu())
            menu->close();
    }
    if (auto* menu = overflow_menu())
        menu->close();

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
        auto width = set_up_bookmark_button(*button, title);

        auto id = ak_string_from_qstring(button->defaultAction() ? button->defaultAction()->property("id").toString() : QString {});
        current_ids.set(id);
        if (animates && !id.is_empty() && !previous_items.contains(id))
            slide_in(*button, width);
    };

    for (auto const& item : Application::bookmarks().menu().items()) {
        item.visit(
            [&](NonnullRefPtr<Action> const& bookmark) {
                if (bookmark->id() != ActionID::BookmarkItem)
                    return;

                auto* action = create_application_action(*this, *bookmark);
                addAction(action);

                if (auto* button = as_if<QToolButton>(widgetForAction(action)))
                    set_button_properties(button, qstring_from_ak_string(bookmark->text()));
            },
            [&](NonnullRefPtr<Menu> const& folder) {
                if (folder->properties().get("type"sv) != "folder"sv)
                    return;

                auto title = qstring_from_ak_string(folder->title());

                auto* submenu = create_application_menu(*this, *folder);
                install_menu_event_filter(this, submenu);

                auto* action = submenu->menuAction();
                action->setIcon(create_chrome_icon(ChromeIcon::Folder, palette()));
                action->setProperty("id", submenu->property("id"));
                action->setProperty("type", submenu->property("type"));
                action->setProperty("target_folder_id", submenu->property("target_folder_id"));
                action->setProperty("parent_folder_id", submenu->property("parent_folder_id"));
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
            [](Separator) {
            });
    }

    if (animates) {
        for (auto const& [id, item] : previous_items) {
            if (!current_ids.contains(id) && !item.image.isNull())
                slide_out(item.index, item.image);
        }
    }
}

int BookmarksBar::set_up_bookmark_button(QToolButton& button, QString const& title)
{
    button.setProperty(BOOKMARK_ITEM_PROPERTY, true);
    button.setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button.ensurePolished();

    auto option = bookmark_button_style_option(button);

    QFontMetrics title_font_metrics(bookmark_title_font(button));
    auto height = BOOKMARK_BUTTON_HEIGHT;
    auto max_size_rect = QRect { 0, 0, BOOKMARK_BUTTON_MAX_WIDTH, height };

    auto layout = bookmark_button_layout(button, option, title, max_size_rect);

    auto text = title_font_metrics.elidedText(title, Qt::ElideRight, layout.available_text_width);
    button.setText(text);

    layout = bookmark_button_layout(button, option, text, max_size_rect);

    button.setFixedWidth(layout.preferred_width);
    button.setFixedHeight(height);

    button.installEventFilter(this);
    return layout.preferred_width;
}

QPixmap BookmarksBar::bookmark_button_image(QIcon const& icon, QString const& title)
{
    QToolButton button { this };
    button.setAutoRaise(true);
    button.setIcon(icon);
    button.setIconSize(iconSize());
    // The button is drawn the way it looks under the pointer.
    button.setProperty(BOOKMARK_DROP_TARGET_PROPERTY, true);
    set_up_bookmark_button(button, title);
    return button.grab();
}

void BookmarksBar::report_context_menu_item() const
{
    Application::bookmarks().set_context_menu_item(m_selected_bookmark_menu_item_id, m_selected_bookmark_menu_target_folder_id, m_selected_bookmark_menu_parent_folder_id);
}

void BookmarksBar::show_context_menu(QPoint position, Optional<BookmarkItem const&> item, Optional<String const&> target_folder_id, Optional<String const&> parent_folder_id)
{
    if (!current_view())
        return;

    if (item.has_value()) {
        m_selected_bookmark_menu_item_id = item->id;
        m_selected_bookmark_menu_target_folder_id = target_folder_id.copy();
        m_selected_bookmark_menu_parent_folder_id = parent_folder_id.copy();
        report_context_menu_item();

        if (item->is_bookmark())
            execute_context_menu_later(m_context_menu, *this, Application::bookmarks().bookmark_context_menu(), position);
        else if (item->is_folder())
            execute_context_menu_later(m_context_menu, *this, Application::bookmarks().bookmark_folder_context_menu(), position);
    } else {
        // NB: Empty space in a folder that about:bookmarks shows adds to that folder, and elsewhere to the bar.
        m_selected_bookmark_menu_item_id = {};
        m_selected_bookmark_menu_target_folder_id = target_folder_id.copy();
        m_selected_bookmark_menu_parent_folder_id = {};
        report_context_menu_item();

        execute_context_menu_later(m_context_menu, *this, Application::bookmarks().bookmarks_bar_context_menu(), position);
    }
}

bool BookmarksBar::eventFilter(QObject* object, QEvent* event)
{
    if (event == m_move_sent_to_menu)
        return false;
    if (is_dragging_bookmark() && handle_drag_event(*object, *event))
        return true;

    if (event->type() == QEvent::Paint) {
        if (auto* button = as_if<QToolButton>(object); button && button->property(BOOKMARK_ITEM_PROPERTY).toBool()) {
            paint_bookmark_button(*button);
            return true;
        }
    }

    if (auto* menu = m_pressed_menu ? as_if<QMenu>(object) : nullptr) {
        if (event->type() == QEvent::Hide && menu == m_pressed_menu)
            cancel_press();
        if (event->type() == QEvent::MouseButtonRelease && as<QMouseEvent>(*event).button() == Qt::LeftButton)
            cancel_press();
    }

    auto is_pressed_object = object == m_pressed_button || (m_pressed_menu && as_if<QMenu>(object));
    if (is_pressed_object && event->type() == QEvent::MouseMove) {
        auto const& mouse_event = as<QMouseEvent>(*event);
        auto offset = mouse_event.globalPosition().toPoint() - m_press_position;

        // Qt also sends moves of its own making, which name no button.
        if (!QGuiApplication::mouseButtons().testFlag(Qt::LeftButton)) {
            cancel_press();
        } else if (is_past_drag_distance(mouse_event)) {
            // A folder that is dragged downward opens instead.
            if (auto button = m_pressed_button; button && button->defaultAction()->menu() && offset.y() > abs(offset.x())) {
                cancel_press();
                show_menu_later(*button);
            } else {
                start_drag();
            }
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
            if (button && button == m_overflow_button)
                folder_menu = overflow_menu();
            if (folder_menu && !folder_menu->isVisible()) {
                close_menu_chain(*menu);
                show_menu_later(*button);
                return true;
            }
        }
    }

    if (event->type() == QEvent::MouseButtonPress) {
        auto& mouse_event = as<QMouseEvent>(*event);
        m_opened_new_tab_on_menu_press = false;

        // The release of an earlier press may have gone to a menu or dialog that opened during it.
        cancel_press();

        if (mouse_event.button() == Qt::LeftButton)
            return handle_left_mouse_click(&mouse_event, object);
        if (mouse_event.button() == Qt::MiddleButton)
            return handle_middle_mouse_click(&mouse_event, object);
        if (mouse_event.button() == Qt::RightButton)
            return handle_right_mouse_click(&mouse_event, object);
    }

    // Qt takes a release whose press it did not see as a choice of the entry.
    if (m_opened_new_tab_on_menu_press && event->type() == QEvent::MouseButtonRelease && as_if<QMenu>(object)) {
        m_opened_new_tab_on_menu_press = false;
        if (auto* popup = as_if<QMenu>(QApplication::activePopupWidget()))
            close_menu_chain(*popup);
        return true;
    }

    if (object == m_pressed_button && event->type() == QEvent::MouseButtonRelease) {
        auto& mouse_event = as<QMouseEvent>(*event);

        if (mouse_event.button() == Qt::LeftButton) {
            auto button = m_pressed_button;
            cancel_press();

            if (button->defaultAction()->menu()) {
                if (button->rect().contains(mouse_event.position().toPoint()))
                    show_menu_later(*button);
                return true;
            }
        }
    }

    return QToolBar::eventFilter(object, event);
}

bool BookmarksBar::handle_left_mouse_click(QMouseEvent* event, QObject* item)
{
    if (event->modifiers().testFlag(Qt::ControlModifier))
        return handle_middle_mouse_click(event, item);

    if (auto* menu = as_if<QMenu>(item)) {
        if (auto* action = menu->actionAt(event->pos())) {
            if (!bookmark_item_id(*action).isEmpty()) {
                m_pressed_menu = menu;
                m_pressed_menu_action = action;
                m_press_position = event->globalPosition().toPoint();
            }
        }
        return false;
    }

    auto* button = as_if<QToolButton>(item);
    if (!button || !button->property(BOOKMARK_ITEM_PROPERTY).toBool())
        return false;

    m_pressed_button = button;
    m_press_position = event->globalPosition().toPoint();

    // A folder opens when its button is released, as a press may be the start of a drag.
    if (button->defaultAction()->menu()) {
        button->setDown(true);
        return true;
    }
    return false;
}

// The translucent picture of a dragged bookmark.
class BookmarkDragCard final : public QWidget {
public:
    AK_ALLOC_WITH_KMALLOC;

    BookmarkDragCard(QWidget& parent, QPixmap pixmap)
        : QWidget(&parent)
        , m_pixmap(AK::move(pixmap))
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        resize(m_pixmap.deviceIndependentSize().toSize());
    }

private:
    virtual void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setOpacity(BOOKMARK_DRAG_IMAGE_OPACITY);
        painter.drawPixmap(0, 0, m_pixmap);
    }

    QPixmap m_pixmap;
};

void BookmarksBar::cancel_press()
{
    if (m_pressed_button && m_pressed_button->defaultAction()->menu())
        m_pressed_button->setDown(false);
    m_pressed_button = nullptr;
    m_pressed_menu = nullptr;
    m_pressed_menu_action = nullptr;
}

bool BookmarksBar::is_past_drag_distance(QMouseEvent const& event) const
{
    auto offset = event.globalPosition().toPoint() - m_press_position;
    return offset.manhattanLength() >= QApplication::startDragDistance();
}

void BookmarksBar::start_drag()
{
    auto dragged_button = m_pressed_button;
    auto dragged_menu_action = m_pressed_menu_action;
    auto* dragged_action = dragged_button ? dragged_button->defaultAction() : dragged_menu_action.data();
    if (!dragged_action)
        return;

    m_drag_source_menu = m_pressed_menu;
    cancel_press();
    if (m_drag_source_menu)
        activate_entries_leading_to(*m_drag_source_menu);

    m_dragged_button = dragged_button;
    if (dragged_button) {
        dragged_button->setDown(false);
        m_drag_card = new BookmarkDragCard(*this, dragged_button->grab());
        m_drag_card_pointer_offset = dragged_button->mapFromGlobal(m_press_position).x();
        m_drag_card->move(dragged_button->pos());
        m_drag_card->show();
        m_drag_card->raise();
    } else {
        // The card shows once the pointer is over the bar.
        m_drag_card = new BookmarkDragCard(*this, bookmark_button_image(dragged_menu_action->icon(), dragged_menu_action->text()));
        m_drag_card_pointer_offset = m_drag_card->width() / 2;
        for (auto* action : actions()) {
            if (auto* button = widgetForAction(action); button && button->property(BOOKMARK_ITEM_PROPERTY).toBool()) {
                m_drag_card->move(0, button->y());
                break;
            }
        }
    }

    m_dragged_item_id = bookmark_item_id(*dragged_action);
    QApplication::setOverrideCursor(Qt::DragMoveCursor);

    // Qt passes moves to different widgets while a button is held, and to none once a menu has opened and closed.
    qApp->installEventFilter(this);
}

bool BookmarksBar::handle_drag_event(QObject const& object, QEvent& event)
{
    switch (event.type()) {
    case QEvent::MouseMove:
    case QEvent::MouseButtonRelease: {
        // Mouse events are followed as their window gets them, and kept from the widgets.
        if (!object.isWindowType())
            return true;

        auto const& mouse_event = as<QMouseEvent>(event);
        auto global_position = mouse_event.globalPosition().toPoint();

        if (event.type() == QEvent::MouseButtonRelease && mouse_event.button() == Qt::LeftButton)
            drop_dragged_item(global_position);
        else if (QGuiApplication::mouseButtons().testFlag(Qt::LeftButton))
            update_drag(global_position);
        else
            stop_drag();
        return false;
    }
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
        stop_drag();
        return true;
    case QEvent::ShortcutOverride:
        event.accept();
        return true;
    case QEvent::KeyPress:
        if (as<QKeyEvent>(event).key() == Qt::Key_Escape)
            stop_drag();
        return true;
    case QEvent::KeyRelease:
        return true;
    case QEvent::ToolTip:
        return true;
    case QEvent::Wheel:
        // A wheel scroll moves the entries of a menu under the pointer.
        if (as_if<QMenu>(&object)) {
            QTimer::singleShot(0, this, [this] {
                if (is_dragging_bookmark())
                    update_drag(m_drag_position);
            });
        }
        return false;
    case QEvent::Close:
        // The menus that the drag started in stay open until the release.
        if (auto const* menu = as_if<QMenu>(&object); menu && is_menu_or_parent_of(*menu, m_drag_source_menu)) {
            event.ignore();
            return true;
        }
        return false;
    default:
        return false;
    }
}

void BookmarksBar::update_drag(QPoint global_position)
{
    m_drag_position = global_position;

    // Qt refills the overflow menu whenever it lays out the bar, which clears the active entry of that menu.
    if (auto* menu = overflow_menu(); menu && menu->isVisible() && !menu->activeAction()) {
        for (auto* action : menu->actions()) {
            if (action->menu() && action->menu()->isVisible()) {
                menu->setActiveAction(action);
                break;
            }
        }
    }

    auto position = mapFromGlobal(global_position);
    auto* menu = drag_menu_at(global_position);
    auto position_in_menu = menu ? menu->mapFromGlobal(global_position) : QPoint {};
    auto is_over_scroll_arrow = menu && menu_scroll_arrow_at(*menu, position_in_menu) != MenuScrollArrow::None;
    if (!is_over_scroll_arrow)
        m_drag_menu_scroll_timer->stop();
    auto is_over_bar = !menu && rect().contains(position);
    // The pointer may cross the end of the bar on its way from the overflow button into its menu.
    auto is_over_overflow_button = false;
    if (is_over_bar && m_overflow_button && m_overflow_button->isVisible()) {
        auto geometry = m_overflow_button->geometry();
        is_over_overflow_button = isRightToLeft() ? position.x() <= geometry.right() : position.x() >= geometry.left();
    }

    if (m_drag_card) {
        m_drag_card->move(position.x() - m_drag_card_pointer_offset, m_drag_card->y());
        m_drag_card->setVisible(is_over_bar && !is_over_overflow_button);
        m_drag_card->raise();
    }

    if (is_over_scroll_arrow) {
        hide_drop_target();
        if (!m_drag_menu_scroll_timer->isActive()) {
            close_submenu_opened_by_drag(*menu);
            // A move over the menu's frame makes it forget its active entry.
            send_move_to_menu(*menu, { 0, menu->height() / 2 });
            update_drag_source_menu_visibility();
            m_drag_menu_scroll_timer->start();
        }
        return;
    }
    if (menu) {
        show_drop_target(drop_target_in_menu(*menu, position_in_menu));
        if (m_drop_target.folder_action && menu_scrolls(*menu)) {
            send_move_to_menu(*menu, position_in_menu);
            update_drag_source_menu_visibility();
        }
        return;
    }
    if (!is_over_bar) {
        hide_drop_target();
        return;
    }
    show_drop_target(drop_target_on_bar(position));

    auto* folder_action = m_drop_target.folder_action.data();
    auto* folder_menu = folder_action ? folder_action->menu() : nullptr;
    auto* folder_button = widgetForAction(folder_action);
    if (is_over_overflow_button) {
        folder_menu = overflow_menu();
        folder_button = m_overflow_button;
    }
    auto is_over_drag_source_folder = folder_menu && is_menu_or_parent_of(*folder_menu, m_drag_source_menu);
    if (folder_menu && folder_menu->isVisible() && !is_over_drag_source_folder)
        return;
    close_menus_opened_by_drag();
    set_drag_source_menu_hidden(!is_over_drag_source_folder);
    if (!folder_menu || folder_menu->isVisible())
        return;

    prepare_menu_to_open_during_drag(*folder_menu);
    m_drag_folder_menu = folder_menu;
    auto x = isRightToLeft() ? folder_button->width() - folder_menu->sizeHint().width() : 0;
    folder_menu->popup(folder_button->mapToGlobal(QPoint { x, folder_button->height() }));
}

void BookmarksBar::prepare_menu_to_open_during_drag(QMenu& menu)
{
    static bool const is_wayland = QGuiApplication::platformName().startsWith(QStringLiteral("wayland"));
    if (!m_drag_source_menu || !is_wayland || menu.windowType() != Qt::Popup || menu.isVisible())
        return;

    m_drag_menus_without_grab.append({ &menu, menu.windowFlags() });
    // On Wayland, a menu can only open away from the menu that the drag started in as a tooltip window.
    menu.setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint);
}

QMenu* BookmarksBar::drag_menu_at(QPoint global_position) const
{
    auto source_menus = drag_source_menus();
    auto* outermost_source_menu = source_menus.is_empty() || m_drag_source_menu_is_hidden ? nullptr : source_menus.first();
    for (auto* outermost_menu : { m_drag_folder_menu.data(), outermost_source_menu }) {
        if (!outermost_menu)
            continue;
        QMenu* menu_at_position = nullptr;
        for (auto* menu = &innermost_open_menu(*outermost_menu); menu; menu = parent_menu(*menu)) {
            if (!menu_at_position && menu->isVisible() && menu->geometry().contains(global_position))
                menu_at_position = menu;
            // A folder cannot be dropped in its own menu or one below it.
            if (menu->property("id").toString() == m_dragged_item_id)
                menu_at_position = nullptr;
        }
        if (menu_at_position)
            return menu_at_position;
    }
    return nullptr;
}

void BookmarksBar::scroll_drag_menu()
{
    auto* menu = drag_menu_at(m_drag_position);
    auto position = menu ? menu->mapFromGlobal(m_drag_position) : QPoint {};
    auto arrow = menu ? menu_scroll_arrow_at(*menu, position) : MenuScrollArrow::None;
    if (arrow == MenuScrollArrow::None) {
        update_drag(m_drag_position);
        return;
    }

    close_submenu_opened_by_drag(*menu);
    update_drag_source_menu_visibility();
    // QApplication drops a wheel event for a menu that is not the latest popup.
    auto delta = arrow == MenuScrollArrow::Up ? QWheelEvent::DefaultDeltasPerStep : -QWheelEvent::DefaultDeltasPerStep;
    QWheelEvent wheel { position, m_drag_position, {}, { 0, delta }, Qt::LeftButton, QGuiApplication::keyboardModifiers(), Qt::NoScrollPhase, false };
    static_cast<QObject*>(menu)->event(&wheel);
}

void BookmarksBar::close_submenu_opened_by_drag(QMenu& menu)
{
    auto source_menus = drag_source_menus();
    auto index = source_menus.find_first_index(&menu);
    auto const* source_submenu = index.has_value() && *index + 1 < source_menus.size() ? source_menus[*index + 1] : nullptr;
    close_open_submenu(menu, source_submenu);
}

void BookmarksBar::send_move_to_menu(QMenu& menu, QPoint position)
{
    QMouseEvent move { QEvent::MouseMove, position, menu.mapToGlobal(position), Qt::NoButton, Qt::NoButton, QGuiApplication::keyboardModifiers() };
    m_move_sent_to_menu = &move;
    QApplication::sendEvent(&menu, &move);
    m_move_sent_to_menu = nullptr;
}

void BookmarksBar::close_menus_opened_by_drag()
{
    if (m_drag_folder_menu)
        close_menu_chain(innermost_open_menu(*m_drag_folder_menu));
    m_drag_folder_menu = nullptr;

    if (!m_drag_source_menu)
        return;
    QMenu const* submenu = nullptr;
    for (auto* menu = m_drag_source_menu.data(); menu; submenu = menu, menu = parent_menu(*menu))
        close_open_submenu(*menu, submenu);
    activate_entries_leading_to(*m_drag_source_menu);
    update_drag_source_menu_visibility();
}

Vector<QMenu*> BookmarksBar::drag_source_menus() const
{
    Vector<QMenu*> menus;
    for (auto* menu = m_drag_source_menu.data(); menu; menu = parent_menu(*menu))
        menus.prepend(menu);
    return menus;
}

void BookmarksBar::set_drag_source_menu_hidden(bool hidden)
{
    if (!m_drag_source_menu || m_drag_source_menu_is_hidden == hidden)
        return;
    m_drag_source_menu_is_hidden = hidden;
    update_drag_source_menu_visibility();

    auto* folder_menu = drag_source_menus().first();
    if (folder_menu == overflow_menu())
        m_overflow_button->setDown(!hidden);
    for (auto* action : actions()) {
        auto* button = action->menu() == folder_menu ? widgetForAction(action) : nullptr;
        if (!button)
            continue;
        button->setProperty(BOOKMARK_FOLDER_MENU_OPEN_PROPERTY, !hidden);
        button->update();
    }
}

void BookmarksBar::update_drag_source_menu_visibility()
{
    auto menus = drag_source_menus();
    auto is_shown = !m_drag_source_menu_is_hidden;
    for (size_t i = 0; i < menus.size(); ++i) {
        if (i > 0)
            is_shown = is_shown && menus[i - 1]->activeAction() == menus[i]->menuAction();
        set_floating_window_opacity(*menus[i], is_shown ? 1 : 0);
        if (!is_shown)
            close_open_submenu(*menus[i], i + 1 < menus.size() ? menus[i + 1] : nullptr);
    }
}

void BookmarksBar::drop_dragged_item(QPoint global_position)
{
    update_drag(global_position);
    auto id = ak_string_from_qstring(m_dragged_item_id);
    auto target = m_drop_target;
    stop_drag();

    auto& store = Application::bookmark_store();

    // The entries of the overflow menu follow the items that the bar shows.
    auto is_in_overflow_menu = target.menu && target.menu == overflow_menu();
    if (is_in_overflow_menu && !target.folder_action)
        target.index += first_overflow_index();

    auto* folder_menu = target.folder_action ? target.folder_action->menu() : target.menu.data();
    if (is_in_overflow_menu && !target.folder_action)
        folder_menu = nullptr;
    auto folder_id = folder_menu ? ak_string_from_qstring(folder_menu->property("id").toString()) : Optional<String> {};

    auto parent_folder = folder_id.has_value() ? store.find_item_by_id(*folder_id) : Optional<BookmarkItem const&> {};
    auto const& siblings = parent_folder.has_value() ? parent_folder->folder().children : store.root_items();
    auto current_index = siblings.find_first_index_if([&](auto const& item) { return item.id == id; });

    if (target.folder_action) {
        if (!current_index.has_value())
            store.move_item(id, folder_id, NumericLimits<size_t>::max());
        return;
    }
    if (target == DropTarget {})
        return;
    if (current_index.has_value() && first_is_one_of(target.index, *current_index, *current_index + 1))
        return;

    store.move_item(id, folder_id, target.index);
}

void BookmarksBar::stop_drag()
{
    if (!is_dragging_bookmark())
        return;

    m_dragged_item_id = {};
    qApp->removeEventFilter(this);
    QApplication::restoreOverrideCursor();
    m_drag_menu_scroll_timer->stop();

    delete m_drag_card;
    m_drag_card = nullptr;
    hide_drop_target();
    update();

    // Qt does not tell the dragged button that the pointer has left it.
    if (m_dragged_button && !m_dragged_button->rect().contains(m_dragged_button->mapFromGlobal(QCursor::pos()))) {
        QEvent leave { QEvent::Leave };
        QApplication::sendEvent(m_dragged_button, &leave);
    }
    // A cancelled drag leaves the dragged button pressed.
    if (m_dragged_button && QGuiApplication::mouseButtons().testFlag(Qt::LeftButton)) {
        QMouseEvent release { QEvent::MouseButtonRelease, QPointF { -1, -1 }, QCursor::pos(), Qt::LeftButton, Qt::NoButton, QGuiApplication::keyboardModifiers() };
        QApplication::sendEvent(m_dragged_button, &release);
    }
    m_dragged_button = nullptr;

    close_menus_opened_by_drag();
    set_drag_source_menu_hidden(false);
    if (m_drag_source_menu)
        close_menu_chain(*m_drag_source_menu);
    m_drag_source_menu = nullptr;
    if (auto* popup = as_if<QMenu>(QApplication::activePopupWidget()))
        close_menu_chain(*popup);

    for (auto const& [menu, usual_window_flags] : m_drag_menus_without_grab) {
        if (menu)
            menu->setWindowFlags(usual_window_flags);
    }
    m_drag_menus_without_grab.clear();
}

BookmarksBar::DropTarget BookmarksBar::drop_target_on_bar(QPoint position) const
{
    auto spacing_around_indicator = (layout()->spacing() - BOOKMARK_DROP_INDICATOR_WIDTH) / 2;
    auto indicator_rect_beside = [&](QRect const& geometry, bool is_at_start) {
        auto is_at_left = is_at_start != isRightToLeft();
        auto x = is_at_left ? geometry.left() - spacing_around_indicator - BOOKMARK_DROP_INDICATOR_WIDTH : geometry.right() + 1 + spacing_around_indicator;
        return QRect { x, geometry.top(), BOOKMARK_DROP_INDICATOR_WIDTH, geometry.height() };
    };

    DropTarget target;
    QToolButton* previous_button = nullptr;

    for (auto* action : actions()) {
        auto id = action->property("id").toString();
        auto* button = as_if<QToolButton>(widgetForAction(action));
        if (id.isEmpty() || !button)
            continue;
        // The items that do not fit on the bar are hidden.
        if (!button->isVisible())
            break;

        auto geometry = button->geometry();

        if (action->menu() && id != m_dragged_item_id) {
            auto edge_width = geometry.width() / 4;
            if (position.x() >= geometry.left() + edge_width && position.x() <= geometry.right() - edge_width) {
                target.folder_action = action;
                return target;
            }
        }

        auto is_before_center = isRightToLeft() ? position.x() > geometry.center().x() : position.x() < geometry.center().x();
        if (is_before_center) {
            target.indicator_rect = indicator_rect_beside(geometry, true);
            return target;
        }

        ++target.index;
        previous_button = button;
    }

    if (previous_button)
        target.indicator_rect = indicator_rect_beside(previous_button->geometry(), false);
    return target;
}

BookmarksBar::DropTarget BookmarksBar::drop_target_in_menu(QMenu& menu, QPoint position) const
{
    DropTarget target;
    target.menu = &menu;

    auto indicator_rect_at = [](QRect entry_geometry, int y) {
        return QRect { entry_geometry.left(), y - BOOKMARK_DROP_INDICATOR_WIDTH / 2, entry_geometry.width(), BOOKMARK_DROP_INDICATOR_WIDTH };
    };

    // A menu with more entries than fit below one another has several columns.
    auto horizontal_distance_to = [&](QRect entry_geometry) {
        return max(max(entry_geometry.left() - position.x(), position.x() - entry_geometry.right()), 0);
    };
    Optional<QRect> nearest_entry_geometry;
    for (auto* action : menu.actions()) {
        if (bookmark_item_id(*action).isEmpty())
            break;
        auto geometry = menu.actionGeometry(action);
        if (!nearest_entry_geometry.has_value() || horizontal_distance_to(geometry) < horizontal_distance_to(*nearest_entry_geometry))
            nearest_entry_geometry = geometry;
    }

    Optional<QRect> previous_entry_geometry;

    for (auto* action : menu.actions()) {
        auto id = bookmark_item_id(*action);
        // The entries that are not bookmarks or folders follow those that are.
        if (id.isEmpty())
            break;

        auto geometry = menu.actionGeometry(action);

        if (geometry.left() != nearest_entry_geometry->left()) {
            if (previous_entry_geometry.has_value())
                break;
            ++target.index;
            continue;
        }

        if (action->menu() && id != m_dragged_item_id) {
            auto edge_height = geometry.height() / 4;
            if (position.y() >= geometry.top() + edge_height && position.y() <= geometry.bottom() - edge_height) {
                target.folder_action = action;
                return target;
            }
        }

        if (position.y() < geometry.center().y()) {
            target.indicator_rect = indicator_rect_at(geometry, geometry.top());
            return target;
        }

        ++target.index;
        previous_entry_geometry = geometry;
    }

    if (previous_entry_geometry.has_value()) {
        target.indicator_rect = indicator_rect_at(*previous_entry_geometry, previous_entry_geometry->bottom() + 1);
    } else if (!menu.actions().isEmpty()) {
        // An empty folder has a placeholder entry.
        auto geometry = menu.actionGeometry(menu.actions().first());
        target.indicator_rect = indicator_rect_at(geometry, geometry.top());
    }
    return target;
}

void BookmarksBar::show_drop_target(DropTarget const& target)
{
    if (target == m_drop_target)
        return;

    hide_drop_target();
    m_drop_target = target;

    if (auto* folder_button = widgetForAction(m_drop_target.folder_action)) {
        folder_button->setProperty(BOOKMARK_DROP_TARGET_PROPERTY, true);
        folder_button->update();
    }

    if (m_drop_target.menu) {
        auto* active_action = m_drop_target.menu->activeAction();
        // Qt would leave the submenu of an entry open after that entry stops being the active one.
        auto has_open_submenu = active_action && active_action->menu() && active_action->menu()->isVisible();
        if (m_drop_target.folder_action || !has_open_submenu) {
            if (m_drop_target.folder_action)
                prepare_menu_to_open_during_drag(*m_drop_target.folder_action->menu());
            // In a menu that scrolls, the pointer's moves activate the entry instead.
            if (!m_drop_target.folder_action || !menu_scrolls(*m_drop_target.menu))
                m_drop_target.menu->setActiveAction(m_drop_target.folder_action);
            update_drag_source_menu_visibility();
        }

        if (!m_drop_target.indicator_rect.isEmpty()) {
            m_menu_drop_indicator = new QWidget(m_drop_target.menu);
            m_menu_drop_indicator->setAttribute(Qt::WA_TransparentForMouseEvents);
            m_menu_drop_indicator->setAutoFillBackground(true);
            QPalette palette;
            palette.setColor(QPalette::Window, Theme::accent());
            m_menu_drop_indicator->setPalette(palette);
            m_menu_drop_indicator->setGeometry(m_drop_target.indicator_rect);
            m_menu_drop_indicator->show();
        }
    }
    update();
}

void BookmarksBar::hide_drop_target()
{
    if (m_drop_target == DropTarget {})
        return;

    if (auto* folder_button = widgetForAction(m_drop_target.folder_action)) {
        folder_button->setProperty(BOOKMARK_DROP_TARGET_PROPERTY, false);
        folder_button->update();
    }
    delete m_menu_drop_indicator;
    m_drop_target = {};
    update();
}

void BookmarksBar::paintEvent(QPaintEvent* event)
{
    QToolBar::paintEvent(event);

    if (!m_drop_target.menu && !m_drop_target.indicator_rect.isEmpty()) {
        QPainter painter(this);
        painter.fillRect(m_drop_target.indicator_rect, Theme::accent());
    }
}

bool BookmarksBar::handle_middle_mouse_click(QMouseEvent* event, QObject* item)
{
    auto activate_tab = event->modifiers().testFlag(Qt::ShiftModifier) ? Web::HTML::ActivateTab::No : Web::HTML::ActivateTab::Yes;

    if (auto* button = as_if<QToolButton>(item)) {
        auto* action = button->defaultAction();
        extract_item_properties(action);

        if (m_selected_bookmark_menu_item_type == "bookmark")
            Application::bookmarks().open_in_new_tab(m_selected_bookmark_menu_item_id, activate_tab);
    } else if (auto* menu = as_if<QMenu>(item)) {
        if (auto* action = menu->actionAt(event->pos())) {
            extract_item_properties(action);

            if (m_selected_bookmark_menu_item_type == "bookmark") {
                Application::bookmarks().open_in_new_tab(m_selected_bookmark_menu_item_id, activate_tab);
                m_opened_new_tab_on_menu_press = true;
            }
        }
    }

    return true;
}

bool BookmarksBar::handle_right_mouse_click(QMouseEvent* event, QObject* item)
{
    // FIXME: The menus below should use execute_context_menu_later() instead to use native context menus on macOS.
    //        However, more work is needed to ensure that opened bookmark folders do not disappear when the native menu
    //        is shown.
    if (!current_view())
        return true;

    auto position = event->globalPosition().toPoint();
    auto context_menu_for_selected_item = [&]() -> Menu* {
        if (m_selected_bookmark_menu_item_type == "bookmark")
            return &Application::bookmarks().bookmark_context_menu();
        if (m_selected_bookmark_menu_item_type == "folder")
            return &Application::bookmarks().bookmark_folder_context_menu();
        return nullptr;
    };

    if (is<BookmarksBar>(item)) {
        m_selected_bookmark_menu_item_id = {};
        m_selected_bookmark_menu_target_folder_id = {};
        m_selected_bookmark_menu_parent_folder_id = {};
        report_context_menu_item();

        replace_context_menu(m_context_menu, *this, Application::bookmarks().bookmarks_bar_context_menu()).popup(position);
    } else if (auto* button = as_if<QToolButton>(item)) {
        extract_item_properties(button->defaultAction());
        auto* menu = context_menu_for_selected_item();
        if (!menu)
            return true;
        report_context_menu_item();

        auto& context_menu = replace_context_menu(m_context_menu, *this, *menu);
        button->setProperty(BOOKMARK_CONTEXT_MENU_OPEN_PROPERTY, true);
        button->update();
        QObject::connect(&context_menu, &QMenu::aboutToHide, button, [button] {
            button->setProperty(BOOKMARK_CONTEXT_MENU_OPEN_PROPERTY, false);
            button->update();
        });
        context_menu.popup(position);
    } else if (auto* dropdown = as_if<QMenu>(item)) {
        if (!dropdown->rect().contains(event->pos()))
            return false;

        QObject* clicked_item = dropdown;
        if (auto* action = dropdown->actionAt(event->pos())) {
            QObject* submenu = action->menu();
            if (auto* entry = submenu ?: action; entry->property("type").isValid())
                clicked_item = entry;
        }
        extract_item_properties(clicked_item);

        auto* menu = context_menu_for_selected_item();
        if (!menu)
            return true;
        report_context_menu_item();

        // NB: The context menu is parented to the dropdown. Otherwise, Qt complains that the context menu's parent does
        //     not match the current topmost popup.
        auto& context_menu = replace_context_menu(m_context_menu, *dropdown, *menu);
        QObject::connect(&context_menu, &QMenu::triggered, dropdown, [dropdown] {
            dropdown->close();
        });
        context_menu.popup(position);
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

// The bookmark menus are the selected tab's.
WebContentView* BookmarksBar::current_view() const
{
    auto* tab = as<BrowserWindow>(*window()).current_tab();
    if (!tab || !tab->is_loaded())
        return nullptr;
    return &tab->view();
}

static constexpr int BOOKMARKS_BAR_HOVER_COLLAPSE_POLL_INTERVAL_MS = 250;

BookmarksBarContainer::BookmarksBarContainer(WindowToolbar& toolbar, QWidget& page_stack, QWidget* parent)
    : QWidget(parent)
    , m_toolbar(toolbar)
    , m_page_stack(page_stack)
{
    setObjectName("LadybirdBookmarksBarContainer");
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
#if defined(AK_OS_MACOS) || defined(LADYBIRD_QT_USE_RHI_WIDGET)
    // The web view is native on these presentation paths, so the bar that can overlay it must also be native.
    setAttribute(Qt::WA_DontCreateNativeAncestors);
    setAttribute(Qt::WA_NativeWindow);
#endif

    m_bar = new BookmarksBar(this);
    m_bar->show();

    m_hover_expand_timer = new QTimer(this);
    m_hover_expand_timer->setSingleShot(true);
    m_hover_expand_timer->setTimerType(Qt::PreciseTimer);
    connect(m_hover_expand_timer, &QTimer::timeout, this, [this] {
        if (cursor_is_over_hover_region())
            set_hover_expanded(true);
    });

    m_hover_expand_animation = new QVariantAnimation(this);
    m_hover_expand_animation->setEasingCurve(QEasingCurve::OutCubic);
    m_hover_expand_animation->setStartValue(0.0);
    m_hover_expand_animation->setEndValue(1.0);
    connect(m_hover_expand_animation, &QVariantAnimation::valueChanged, this, [this](QVariant const& value) {
        m_hover_expand_progress = value.toReal();
        update_geometry();
    });

    m_hover_collapse_timer = new QTimer(this);
    m_hover_collapse_timer->setInterval(BOOKMARKS_BAR_HOVER_COLLAPSE_POLL_INTERVAL_MS);
    connect(m_hover_collapse_timer, &QTimer::timeout, this, &BookmarksBarContainer::update_hover);

    m_toolbar.installEventFilter(this);
    m_page_stack.installEventFilter(this);
    m_bar->installEventFilter(this);
    installEventFilter(this);
    update_geometry();
}

void BookmarksBarContainer::bind(Tab* tab)
{
    if (m_tab) {
        QObject::disconnect(m_tab, nullptr, this, nullptr);
        if (m_tab->is_loaded()) {
            m_tab->location_edit().removeEventFilter(this);
            m_tab->view().set_bookmarks_bar_overlay_height(0);
        }
    }
    m_tab = tab;
    set_hover_expanded(false);

    // A tab that hasn't loaded has no page yet for the bar to lie over.
    if (tab && !tab->is_loaded()) {
        QObject::connect(tab, &Tab::loaded, this, [this, tab] { bind(tab); }, Qt::SingleShotConnection);
        return;
    }
    if (tab)
        tab->location_edit().installEventFilter(this);
    update_geometry();
}

void BookmarksBarContainer::set_sidebar(QWidget& sidebar)
{
    m_sidebar = &sidebar;
    m_sidebar->installEventFilter(this);
}

bool BookmarksBarContainer::shows_over_page() const
{
    return Application::settings().appearance().show_bookmarks_bar == WebView::ShowBookmarksBar::OnHover;
}

bool BookmarksBarContainer::can_expand_on_hover() const
{
    return shows_over_page() && m_page_stack.isVisible() && m_toolbar.isVisible() && !window()->isFullScreen()
        && !(m_tab && m_tab->is_loaded() && m_tab->location_edit().property("autocompleteOpen").toBool());
}

bool BookmarksBarContainer::cursor_is_over_hover_region() const
{
    if (m_bar->is_dragging_bookmark())
        return true;

    // Menus are separate windows, so QWidget::isAncestorOf() does not follow them back to the toolbar.
    for (auto* popup = QApplication::activePopupWidget(); popup; popup = popup->parentWidget()) {
        if (popup == m_bar)
            return true;
    }

    // NB: The page can be presented by a native window of its own, which takes the pointer from the browser window.
    auto* widget_under_cursor = QApplication::widgetAt(QCursor::pos());
    if (!window()->underMouse() && !(widget_under_cursor && widget_under_cursor->window() == window()))
        return false;

    auto cursor_position = QCursor::pos();
    auto contains_cursor = [&](QWidget const& widget) {
        return widget.isVisible() && QRect { widget.mapToGlobal(QPoint {}), widget.size() }.contains(cursor_position);
    };

    return contains_cursor(m_toolbar) || contains_cursor(*this)
        || (m_sidebar && m_toolbar.arrangement() == ToolbarArrangement::Sidebar && contains_cursor(*m_sidebar));
}

void BookmarksBarContainer::update_hover()
{
    if (!can_expand_on_hover() || !cursor_is_over_hover_region()) {
        set_hover_expanded(false);
        return;
    }

    if (!m_hover_expanded && !m_hover_expand_timer->isActive()) {
        auto delay = Application::settings().config_variable_as_integer<int>(WebView::ConfigVariableID::VerticalTabsHoverExpandDelay);
        m_hover_expand_timer->start(delay);
    }
}

void BookmarksBarContainer::set_hover_expanded(bool expanded)
{
    m_hover_expand_timer->stop();
    expanded &= can_expand_on_hover();
    if (m_hover_expanded == expanded)
        return;

    m_hover_expanded = expanded;
    m_hover_expand_animation->stop();

    auto duration = expanded && !SystemAppearance::prefers_reduced_motion()
        ? Application::settings().config_variable_as_integer<int>(WebView::ConfigVariableID::VerticalTabsHoverExpandAnimationDuration)
        : 0;
    m_hover_expand_progress = expanded && duration == 0 ? 1.0 : 0.0;

    if (expanded)
        m_hover_collapse_timer->start();
    else
        m_hover_collapse_timer->stop();

    update_geometry();

    if (expanded && duration > 0) {
        m_hover_expand_animation->setDuration(duration);
        m_hover_expand_animation->start();
    }
}

void BookmarksBarContainer::update_geometry()
{
    auto show_bookmarks_bar = Application::settings().appearance().show_bookmarks_bar;
    auto on_hover = show_bookmarks_bar == WebView::ShowBookmarksBar::OnHover;
    auto visible = !window()->isFullScreen()
        && (show_bookmarks_bar == WebView::ShowBookmarksBar::Always
            || (show_bookmarks_bar == WebView::ShowBookmarksBar::OnHover && m_hover_expanded));

    auto full_height = m_bar->sizeHint().height();
    auto height = on_hover
        ? min(qRound(full_height * m_hover_expand_progress), m_page_stack.height())
        : full_height;

    // Clip the full-height toolbar as it slides down, keeping its buttons at their normal size throughout the reveal.
    setFixedHeight(height);
    if (on_hover)
        setGeometry(m_page_stack.x(), m_page_stack.y(), m_page_stack.width(), height);
    m_bar->setGeometry(0, height - full_height, width(), full_height);
    set_view_overlay_height(on_hover && visible ? height : 0);
    setVisible(visible);
    if (on_hover && visible)
        raise();

#if defined(AK_OS_MACOS)
    ensure_appkit_window_is_exposed(*this);
#endif

    m_toolbar.set_bookmarks_bar_below(visible);
}

void BookmarksBarContainer::set_view_overlay_height(int height)
{
    if (m_tab && m_tab->is_loaded())
        m_tab->view().set_bookmarks_bar_overlay_height(height);
}

bool BookmarksBarContainer::event(QEvent* event)
{
    if (event->type() == QEvent::PaletteChange)
        m_bar->update_chrome_style();
    return QWidget::event(event);
}

bool BookmarksBarContainer::eventFilter(QObject* watched, QEvent* event)
{
    auto watched_location_edit = m_tab && m_tab->is_loaded() && watched == &m_tab->location_edit();

    if (watched_location_edit && event->type() == QEvent::DynamicPropertyChange) {
        if (static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName() == "autocompleteOpen")
            update_hover();
    }
    if (watched == &m_page_stack && (event->type() == QEvent::Resize || event->type() == QEvent::Move))
        update_geometry();
    if (watched == this && event->type() == QEvent::Resize)
        update_geometry();
    if (watched == m_bar && event->type() == QEvent::LayoutRequest)
        update_geometry();

    if (watched == &m_toolbar || watched == this || watched == m_sidebar || watched_location_edit) {
        if (event->type() == QEvent::Enter || event->type() == QEvent::Show) {
            update_hover();
        } else if (event->type() == QEvent::Leave) {
            QTimer::singleShot(0, this, &BookmarksBarContainer::update_hover);
        } else if (event->type() == QEvent::Hide && watched == &m_toolbar) {
            set_hover_expanded(false);
        }
    }

    return QWidget::eventFilter(watched, event);
}

}
