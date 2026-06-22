/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/ByteBuffer.h>
#include <AK/Format.h>
#include <AK/LexicalPath.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Types.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Resource.h>
#include <LibCore/Timer.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibGfx/Palette.h>
#include <LibGfx/Rect.h>
#include <LibGfx/SystemTheme.h>
#include <LibWeb/UIEvents/KeyCode.h>
#include <LibWeb/UIEvents/MouseButton.h>
#include <LibWebView/Application.h>
#include <LibWebView/PlatformColors.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/WebContentClient.h>
#include <UI/Qt/Application.h>
#ifdef AK_OS_MACOS
#    include <UI/Qt/MacWindow.h>
#endif
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/WebContentView.h>

#include <QApplication>
#include <QCursor>
#include <QGuiApplication>
#include <QIcon>
#include <QKeySequence>
#include <QMimeData>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QScrollBar>
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
#    include <QStyleHints>
#endif
#include <QTextEdit>
#include <QTimer>
#include <QToolTip>

namespace Ladybird {

bool is_using_dark_system_theme(QWidget&);

static QWidget* initial_web_content_view_parent([[maybe_unused]] QWidget* window)
{
#ifdef AK_OS_MACOS
    return nullptr;
#else
    return window;
#endif
}

WebContentView::WebContentView(QWidget* window, RefPtr<WebView::WebContentClient> parent_client, size_t page_index, WebContentViewInitialState initial_state)
    : WebContentViewBase(initial_web_content_view_parent(window))
{
#ifdef LADYBIRD_QT_USE_METAL_RHI_WIDGET
    // Keep the QRhiWidget out of the top-level QWidget backing store. If it is
    // parented before becoming native, Qt propagates its RHI config to the whole
    // browser window and uploads the full backing store texture on chrome repaints.
    setAttribute(Qt::WA_DontCreateNativeAncestors);
    setAttribute(Qt::WA_NativeWindow);
    setParent(window);
    setApi(QRhiWidget::Api::Metal);
#endif

    m_client_state.client = parent_client;
    m_client_state.page_index = page_index;

    setAttribute(Qt::WA_InputMethodEnabled, true);
    setMouseTracking(true);
    setAcceptDrops(true);

    setFocusPolicy(Qt::FocusPolicy::StrongFocus);

#ifdef LADYBIRD_QT_USE_VULKAN_WINDOW
    create_vulkan_window();
#endif

    m_device_pixel_ratio = devicePixelRatio();
    m_maximum_frames_per_second = initial_state.maximum_frames_per_second;
    m_display_id = initial_state.display_id;
    set_page_background_color_to_system_canvas(is_using_dark_system_theme(*this));

    QObject::connect(qGuiApp, &QGuiApplication::screenRemoved, [this](QScreen*) {
        update_screen_rects();
    });

    QObject::connect(qGuiApp, &QGuiApplication::screenAdded, [this](QScreen*) {
        update_screen_rects();
    });

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        QTimer::singleShot(0, this, [this] {
            update_palette();
            schedule_repaint();
        });
    });
#endif

    m_tooltip_hover_timer.setSingleShot(true);

    QObject::connect(&m_tooltip_hover_timer, &QTimer::timeout, [this] {
        if (m_tooltip_text.has_value())
            QToolTip::showText(
                QCursor::pos(),
                qstring_from_ak_string(m_tooltip_text.value()),
                this);
    });

    initialize_client((parent_client == nullptr) ? CreateNewClient::Yes : CreateNewClient::No);

    on_ready_to_paint = [this]() {
        schedule_repaint();
    };

    on_cursor_change = [this](auto cursor) {
        update_cursor(cursor);
    };

    on_request_tooltip_override = [this](auto position, auto const& tooltip) {
        m_tooltip_override = true;
        if (m_tooltip_hover_timer.isActive())
            m_tooltip_hover_timer.stop();

        auto tooltip_without_carriage_return = tooltip.contains("\r"sv)
            ? tooltip.replace("\r\n"sv, "\n"sv, ReplaceMode::All).replace("\r"sv, "\n"sv, ReplaceMode::All)
            : tooltip;
        QToolTip::showText(
            mapToGlobal(QPoint(position.x(), position.y())),
            qstring_from_ak_string(tooltip_without_carriage_return),
            this);
    };

    on_stop_tooltip_override = [this]() {
        m_tooltip_override = false;
    };

    on_enter_tooltip_area = [this](auto const& tooltip) {
        m_tooltip_text = tooltip.contains("\r"sv)
            ? tooltip.replace("\r\n"sv, "\n"sv, ReplaceMode::All).replace("\r"sv, "\n"sv, ReplaceMode::All)
            : tooltip;
    };

    on_leave_tooltip_area = [this]() {
        m_tooltip_text.clear();
    };

    on_finish_handling_key_event = [this](auto const& event) {
        finish_handling_key_event(event);
    };

    on_finish_handling_drag_event = [this](auto const& event) {
        finish_handling_drag_event(event);
    };

    m_select_dropdown = new QMenu("Select Dropdown", this);
    QObject::connect(m_select_dropdown, &QMenu::aboutToHide, this, [this]() {
        if (!m_select_dropdown->activeAction())
            select_dropdown_closed({});
    });

    on_request_select_dropdown = [this](Gfx::IntPoint content_position, i32 minimum_width, Vector<Web::HTML::SelectItem> items) {
        m_select_dropdown->clear();
        m_select_dropdown->setMinimumWidth(minimum_width);

        auto add_menu_item = [this](Web::HTML::SelectItemOption const& item_option, bool in_option_group) {
            auto label = in_option_group ? qformatted("    {}", item_option.label) : qstring_from_ak_string(item_option.label);

            QAction* action = new QAction(label, this);
            action->setCheckable(true);
            action->setChecked(item_option.selected);
            action->setDisabled(item_option.disabled);
            action->setData(QVariant(static_cast<uint>(item_option.id)));
            QObject::connect(action, &QAction::triggered, this, &WebContentView::select_dropdown_action);
            m_select_dropdown->addAction(action);
        };

        for (auto const& item : items) {
            if (item.has<Web::HTML::SelectItemOptionGroup>()) {
                auto const& item_option_group = item.get<Web::HTML::SelectItemOptionGroup>();
                QAction* subtitle = new QAction(qstring_from_ak_string(item_option_group.label), this);
                subtitle->setDisabled(true);
                m_select_dropdown->addAction(subtitle);

                for (auto const& item_option : item_option_group.items)
                    add_menu_item(item_option, true);
            }

            if (item.has<Web::HTML::SelectItemOption>())
                add_menu_item(item.get<Web::HTML::SelectItemOption>(), false);

            if (item.has<Web::HTML::SelectItemSeparator>())
                m_select_dropdown->addSeparator();
        }

        m_select_dropdown->exec(map_point_to_global_position(content_position));
    };
}

WebContentView::~WebContentView()
{
#ifdef AK_OS_MACOS
    release_metal_resources();
#elif defined(LADYBIRD_QT_USE_VULKAN_WINDOW)
    destroy_vulkan_window();
#endif
}

void WebContentView::select_dropdown_action()
{
    QAction* action = qobject_cast<QAction*>(sender());
    select_dropdown_closed(action->data().value<uint>());
}

static Web::UIEvents::MouseButton get_button_from_qt_mouse_button(Qt::MouseButton button)
{
    if (button == Qt::MouseButton::LeftButton)
        return Web::UIEvents::MouseButton::Primary;
    if (button == Qt::MouseButton::RightButton)
        return Web::UIEvents::MouseButton::Secondary;
    if (button == Qt::MouseButton::MiddleButton)
        return Web::UIEvents::MouseButton::Middle;
    if (button == Qt::MouseButton::BackButton)
        return Web::UIEvents::MouseButton::Backward;
    if (button == Qt::MouseButton::ForwardButton)
        return Web::UIEvents::MouseButton::Forward;
    return Web::UIEvents::MouseButton::None;
}

static Web::UIEvents::MouseButton get_buttons_from_qt_mouse_buttons(Qt::MouseButtons buttons)
{
    auto result = Web::UIEvents::MouseButton::None;
    if (buttons.testFlag(Qt::MouseButton::LeftButton))
        result |= Web::UIEvents::MouseButton::Primary;
    if (buttons.testFlag(Qt::MouseButton::RightButton))
        result |= Web::UIEvents::MouseButton::Secondary;
    if (buttons.testFlag(Qt::MouseButton::MiddleButton))
        result |= Web::UIEvents::MouseButton::Middle;
    if (buttons.testFlag(Qt::MouseButton::BackButton))
        result |= Web::UIEvents::MouseButton::Backward;
    if (buttons.testFlag(Qt::MouseButton::ForwardButton))
        result |= Web::UIEvents::MouseButton::Forward;
    return result;
}

static Web::UIEvents::KeyModifier get_modifiers_from_qt_keyboard_modifiers(Qt::KeyboardModifiers modifiers)
{
    auto result = Web::UIEvents::KeyModifier::Mod_None;
    if (modifiers.testFlag(Qt::AltModifier))
        result |= Web::UIEvents::KeyModifier::Mod_Alt;
    if (modifiers.testFlag(Qt::ShiftModifier))
        result |= Web::UIEvents::KeyModifier::Mod_Shift;
#if defined(AK_OS_MACOS)
    if (modifiers.testFlag(Qt::ControlModifier))
        result |= Web::UIEvents::KeyModifier::Mod_Super;
    if (modifiers.testFlag(Qt::MetaModifier))
        result |= Web::UIEvents::KeyModifier::Mod_Ctrl;
#else
    if (modifiers.testFlag(Qt::ControlModifier))
        result |= Web::UIEvents::KeyModifier::Mod_Ctrl;
    if (modifiers.testFlag(Qt::MetaModifier))
        result |= Web::UIEvents::KeyModifier::Mod_Super;
#endif
    return result;
}

static Web::UIEvents::KeyModifier get_modifiers_from_qt_key_event(QKeyEvent const& event)
{
    auto modifiers = get_modifiers_from_qt_keyboard_modifiers(event.modifiers());
    if (event.modifiers().testFlag(Qt::KeypadModifier))
        modifiers |= Web::UIEvents::KeyModifier::Mod_Keypad;
    return modifiers;
}

static Web::UIEvents::KeyCode get_keycode_from_qt_key_event(QKeyEvent const& event)
{
    struct Mapping {
        constexpr Mapping(Qt::Key q, Web::UIEvents::KeyCode s)
            : qt_key(q)
            , serenity_key(s)
        {
        }

        Qt::Key qt_key;
        Web::UIEvents::KeyCode serenity_key;
    };

    // FIXME: Qt does not differentiate between left-and-right modifier keys. Unfortunately, it seems like we would have
    //        to inspect event.nativeScanCode() / event.nativeVirtualKey() to do so, which has platform-dependent values.
    //        For now, we default to left keys.

    // https://doc.qt.io/qt-6/qt.html#Key-enum
    static constexpr Mapping mappings[] = {
        { Qt::Key_0, Web::UIEvents::Key_0 },
        { Qt::Key_1, Web::UIEvents::Key_1 },
        { Qt::Key_2, Web::UIEvents::Key_2 },
        { Qt::Key_3, Web::UIEvents::Key_3 },
        { Qt::Key_4, Web::UIEvents::Key_4 },
        { Qt::Key_5, Web::UIEvents::Key_5 },
        { Qt::Key_6, Web::UIEvents::Key_6 },
        { Qt::Key_7, Web::UIEvents::Key_7 },
        { Qt::Key_8, Web::UIEvents::Key_8 },
        { Qt::Key_9, Web::UIEvents::Key_9 },
        { Qt::Key_A, Web::UIEvents::Key_A },
        { Qt::Key_Alt, Web::UIEvents::Key_LeftAlt },
        { Qt::Key_Ampersand, Web::UIEvents::Key_Ampersand },
        { Qt::Key_Apostrophe, Web::UIEvents::Key_Apostrophe },
        { Qt::Key_AsciiCircum, Web::UIEvents::Key_Circumflex },
        { Qt::Key_AsciiTilde, Web::UIEvents::Key_Tilde },
        { Qt::Key_Asterisk, Web::UIEvents::Key_Asterisk },
        { Qt::Key_At, Web::UIEvents::Key_AtSign },
        { Qt::Key_B, Web::UIEvents::Key_B },
        { Qt::Key_Backslash, Web::UIEvents::Key_Backslash },
        { Qt::Key_Backspace, Web::UIEvents::Key_Backspace },
        { Qt::Key_Bar, Web::UIEvents::Key_Pipe },
        { Qt::Key_BraceLeft, Web::UIEvents::Key_LeftBrace },
        { Qt::Key_BraceRight, Web::UIEvents::Key_RightBrace },
        { Qt::Key_BracketLeft, Web::UIEvents::Key_LeftBracket },
        { Qt::Key_BracketRight, Web::UIEvents::Key_RightBracket },
        { Qt::Key_C, Web::UIEvents::Key_C },
        { Qt::Key_CapsLock, Web::UIEvents::Key_CapsLock },
        { Qt::Key_Colon, Web::UIEvents::Key_Colon },
        { Qt::Key_Comma, Web::UIEvents::Key_Comma },
        { Qt::Key_Control, Web::UIEvents::Key_LeftControl },
        { Qt::Key_D, Web::UIEvents::Key_D },
        { Qt::Key_Delete, Web::UIEvents::Key_Delete },
        { Qt::Key_Dollar, Web::UIEvents::Key_Dollar },
        { Qt::Key_Down, Web::UIEvents::Key_Down },
        { Qt::Key_E, Web::UIEvents::Key_E },
        { Qt::Key_End, Web::UIEvents::Key_End },
        { Qt::Key_Equal, Web::UIEvents::Key_Equal },
        { Qt::Key_Enter, Web::UIEvents::Key_Return },
        { Qt::Key_Escape, Web::UIEvents::Key_Escape },
        { Qt::Key_Exclam, Web::UIEvents::Key_ExclamationPoint },
        { Qt::Key_exclamdown, Web::UIEvents::Key_ExclamationPoint },
        { Qt::Key_F, Web::UIEvents::Key_F },
        { Qt::Key_F1, Web::UIEvents::Key_F1 },
        { Qt::Key_F10, Web::UIEvents::Key_F10 },
        { Qt::Key_F11, Web::UIEvents::Key_F11 },
        { Qt::Key_F12, Web::UIEvents::Key_F12 },
        { Qt::Key_F2, Web::UIEvents::Key_F2 },
        { Qt::Key_F3, Web::UIEvents::Key_F3 },
        { Qt::Key_F4, Web::UIEvents::Key_F4 },
        { Qt::Key_F5, Web::UIEvents::Key_F5 },
        { Qt::Key_F6, Web::UIEvents::Key_F6 },
        { Qt::Key_F7, Web::UIEvents::Key_F7 },
        { Qt::Key_F8, Web::UIEvents::Key_F8 },
        { Qt::Key_F9, Web::UIEvents::Key_F9 },
        { Qt::Key_G, Web::UIEvents::Key_G },
        { Qt::Key_Greater, Web::UIEvents::Key_GreaterThan },
        { Qt::Key_H, Web::UIEvents::Key_H },
        { Qt::Key_Home, Web::UIEvents::Key_Home },
        { Qt::Key_I, Web::UIEvents::Key_I },
        { Qt::Key_Insert, Web::UIEvents::Key_Insert },
        { Qt::Key_J, Web::UIEvents::Key_J },
        { Qt::Key_K, Web::UIEvents::Key_K },
        { Qt::Key_L, Web::UIEvents::Key_L },
        { Qt::Key_Left, Web::UIEvents::Key_Left },
        { Qt::Key_Less, Web::UIEvents::Key_LessThan },
        { Qt::Key_M, Web::UIEvents::Key_M },
        { Qt::Key_Menu, Web::UIEvents::Key_Menu },
        { Qt::Key_Meta, Web::UIEvents::Key_LeftSuper },
        { Qt::Key_Minus, Web::UIEvents::Key_Minus },
        { Qt::Key_N, Web::UIEvents::Key_N },
        { Qt::Key_NumberSign, Web::UIEvents::Key_Hashtag },
        { Qt::Key_NumLock, Web::UIEvents::Key_NumLock },
        { Qt::Key_O, Web::UIEvents::Key_O },
        { Qt::Key_P, Web::UIEvents::Key_P },
        { Qt::Key_PageDown, Web::UIEvents::Key_PageDown },
        { Qt::Key_PageUp, Web::UIEvents::Key_PageUp },
        { Qt::Key_ParenLeft, Web::UIEvents::Key_LeftParen },
        { Qt::Key_ParenRight, Web::UIEvents::Key_RightParen },
        { Qt::Key_Percent, Web::UIEvents::Key_Percent },
        { Qt::Key_Period, Web::UIEvents::Key_Period },
        { Qt::Key_Plus, Web::UIEvents::Key_Plus },
        { Qt::Key_Print, Web::UIEvents::Key_PrintScreen },
        { Qt::Key_Q, Web::UIEvents::Key_Q },
        { Qt::Key_Question, Web::UIEvents::Key_QuestionMark },
        { Qt::Key_QuoteDbl, Web::UIEvents::Key_DoubleQuote },
        { Qt::Key_QuoteLeft, Web::UIEvents::Key_Backtick },
        { Qt::Key_R, Web::UIEvents::Key_R },
        { Qt::Key_Return, Web::UIEvents::Key_Return },
        { Qt::Key_Right, Web::UIEvents::Key_Right },
        { Qt::Key_S, Web::UIEvents::Key_S },
        { Qt::Key_ScrollLock, Web::UIEvents::Key_ScrollLock },
        { Qt::Key_Semicolon, Web::UIEvents::Key_Semicolon },
        { Qt::Key_Shift, Web::UIEvents::Key_LeftShift },
        { Qt::Key_Slash, Web::UIEvents::Key_Slash },
        { Qt::Key_Space, Web::UIEvents::Key_Space },
        { Qt::Key_Super_L, Web::UIEvents::Key_LeftSuper },
        { Qt::Key_Super_R, Web::UIEvents::Key_RightSuper },
        { Qt::Key_SysReq, Web::UIEvents::Key_SysRq },
        { Qt::Key_T, Web::UIEvents::Key_T },
        { Qt::Key_Tab, Web::UIEvents::Key_Tab },
        { Qt::Key_U, Web::UIEvents::Key_U },
        { Qt::Key_Underscore, Web::UIEvents::Key_Underscore },
        { Qt::Key_Up, Web::UIEvents::Key_Up },
        { Qt::Key_V, Web::UIEvents::Key_V },
        { Qt::Key_W, Web::UIEvents::Key_W },
        { Qt::Key_X, Web::UIEvents::Key_X },
        { Qt::Key_Y, Web::UIEvents::Key_Y },
        { Qt::Key_Z, Web::UIEvents::Key_Z },
    };

    for (auto const& mapping : mappings) {
        if (event.key() == mapping.qt_key)
            return mapping.serenity_key;
    }
    return Web::UIEvents::Key_Invalid;
}

static bool is_browser_reserved_shortcut(QKeyEvent const& event)
{
    // Browser chrome shortcuts that manage tabs, windows, or focus should not wait for
    // WebContent to decide whether the page wants to suppress them.
    if (event.matches(QKeySequence::StandardKey::AddTab)
        || event.matches(QKeySequence::StandardKey::Close)
        || event.matches(QKeySequence::StandardKey::New)
        || event.matches(QKeySequence::StandardKey::Quit))
        return true;

    auto const modifiers = event.modifiers() & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier);
    auto const key = event.key();

    if (modifiers == (Qt::ControlModifier | Qt::ShiftModifier) && key == Qt::Key_T)
        return true;

    if (modifiers == Qt::ControlModifier && (key == Qt::Key_L || key == Qt::Key_Tab || key == Qt::Key_PageDown))
        return true;

    if (modifiers == (Qt::ControlModifier | Qt::ShiftModifier) && (key == Qt::Key_Tab || key == Qt::Key_Backtab))
        return true;

#if defined(AK_OS_MACOS)
    if (modifiers == Qt::MetaModifier && key == Qt::Key_Tab)
        return true;

    if (modifiers == (Qt::MetaModifier | Qt::ShiftModifier) && (key == Qt::Key_Tab || key == Qt::Key_Backtab))
        return true;

    if (modifiers == (Qt::ControlModifier | Qt::ShiftModifier) && (key == Qt::Key_BracketLeft || key == Qt::Key_BracketRight))
        return true;
#endif

    if (modifiers == Qt::ControlModifier && key == Qt::Key_PageUp)
        return true;

    return false;
}

void WebContentView::keyPressEvent(QKeyEvent* event)
{
    if (is_node_picker_active()) {
        if (event->key() == Qt::Key_Escape)
            node_picker_cancel();
        event->accept();
        return;
    }

    enqueue_native_event(Web::KeyEvent::Type::KeyDown, *event);
}

void WebContentView::keyReleaseEvent(QKeyEvent* event)
{
    if (is_node_picker_active()) {
        event->accept();
        return;
    }

    enqueue_native_event(Web::KeyEvent::Type::KeyUp, *event);
}

void WebContentView::inputMethodEvent(QInputMethodEvent* event)
{
    if (is_node_picker_active()) {
        event->accept();
        return;
    }

    if (!event->commitString().isEmpty()) {
        QKeyEvent keyEvent(QEvent::KeyPress, 0, Qt::NoModifier, event->commitString());
        keyPressEvent(&keyEvent);
    }
    event->accept();
}

QVariant WebContentView::inputMethodQuery(Qt::InputMethodQuery) const
{
    return QVariant();
}

void WebContentView::leaveEvent(QEvent* event)
{
    if (is_node_picker_active()) {
        clear_node_picker();
        WebContentViewBase::leaveEvent(event);
        return;
    }

    static QMouseEvent mouse_event { QEvent::Type::Leave, {}, {}, Qt::MouseButton::NoButton, Qt::MouseButton::NoButton, Qt::KeyboardModifier::NoModifier };
    enqueue_native_event(Web::MouseEvent::Type::MouseLeave, mouse_event);

    WebContentViewBase::leaveEvent(event);
}

void WebContentView::mouseMoveEvent(QMouseEvent* event)
{
    if (is_node_picker_active()) {
        node_picker_hover(node_picker_position_for(*event));
        event->accept();
        return;
    }

    if (!m_tooltip_override) {
        if (QToolTip::isVisible())
            QToolTip::hideText();
        m_tooltip_hover_timer.start(600);
    }

    enqueue_native_event(Web::MouseEvent::Type::MouseMove, *event);
    WebContentViewBase::mouseMoveEvent(event);
}

void WebContentView::mousePressEvent(QMouseEvent* event)
{
    if (is_node_picker_active()) {
        if (event->button() == Qt::MouseButton::LeftButton) {
            auto position = node_picker_position_for(*event);
            if (event->modifiers().testFlag(Qt::ControlModifier))
                node_picker_preview(position);
            else
                node_picker_pick(position);
        }
        event->accept();
        return;
    }

    auto elapsed = event->timestamp() - m_last_click_timestamp;
    auto distance = (event->position() - m_last_click_position).manhattanLength();

    if (elapsed < static_cast<u64>(QApplication::doubleClickInterval()) && distance < QApplication::startDragDistance()) {
        ++m_click_count;
        if (m_click_count < 1)
            m_click_count = 1;
    } else {
        m_click_count = 1;
    }
    m_last_click_timestamp = event->timestamp();
    m_last_click_position = event->position();

    enqueue_native_event(Web::MouseEvent::Type::MouseDown, *event);
}

void WebContentView::mouseReleaseEvent(QMouseEvent* event)
{
    if (is_node_picker_active()) {
        event->accept();
        return;
    }

    enqueue_native_event(Web::MouseEvent::Type::MouseUp, *event);

    if (event->button() == Qt::MouseButton::BackButton)
        (void)traverse_the_history_by_delta(-1);
    else if (event->button() == Qt::MouseButton::ForwardButton)
        (void)traverse_the_history_by_delta(1);
}

void WebContentView::wheelEvent(QWheelEvent* event)
{
    if (is_node_picker_active()) {
        event->accept();
        return;
    }

    if (event->modifiers().testFlag(Qt::ControlModifier)) {
        event->ignore();
        return;
    }

    enqueue_native_event(Web::MouseEvent::Type::MouseWheel, *event);
}

void WebContentView::mouseDoubleClickEvent(QMouseEvent* event)
{
    // NOTE: Qt calls this instead of mousePressEvent on the 2nd click. Forward to mousePressEvent so our click
    //       counting logic handles double and triple clicks uniformly.
    mousePressEvent(event);
}

void WebContentView::dragEnterEvent(QDragEnterEvent* event)
{
    if (is_node_picker_active()) {
        event->ignore();
        return;
    }

    if (!event->mimeData()->hasUrls())
        return;

    enqueue_native_event(Web::DragEvent::Type::DragStart, *event);
    event->acceptProposedAction();
}

void WebContentView::dragMoveEvent(QDragMoveEvent* event)
{
    if (is_node_picker_active()) {
        event->ignore();
        return;
    }

    enqueue_native_event(Web::DragEvent::Type::DragMove, *event);
    event->acceptProposedAction();
}

void WebContentView::dragLeaveEvent(QDragLeaveEvent*)
{
    if (is_node_picker_active())
        return;

    // QDragLeaveEvent does not contain any mouse position or button information.
    Web::DragEvent event {};
    event.type = Web::DragEvent::Type::DragEnd;

    enqueue_input_event(AK::move(event));
}

void WebContentView::dropEvent(QDropEvent* event)
{
    if (is_node_picker_active()) {
        event->ignore();
        return;
    }

    enqueue_native_event(Web::DragEvent::Type::Drop, *event);
    event->acceptProposedAction();
}

void WebContentView::focusInEvent(QFocusEvent*)
{
    client().async_set_has_focus(m_client_state.page_index, true);
}

void WebContentView::focusOutEvent(QFocusEvent*)
{
    client().async_set_has_focus(m_client_state.page_index, false);
}

Optional<WebContentView::Paintable> WebContentView::current_paintable() const
{
    Gfx::SharedImageBuffer const* shared_image_buffer = nullptr;
    Gfx::IntSize bitmap_size;

    if (m_client_state.has_usable_bitmap) {
        VERIFY(m_client_state.front_bitmap.shared_image_buffer);
        shared_image_buffer = m_client_state.front_bitmap.shared_image_buffer.ptr();
        bitmap_size = m_client_state.front_bitmap.last_painted_size.to_type<int>();
    } else if (m_backup_shared_image_buffer) {
        shared_image_buffer = m_backup_shared_image_buffer.ptr();
        bitmap_size = m_backup_bitmap_size.to_type<int>();
    }

    if (!shared_image_buffer)
        return {};
    return Paintable { shared_image_buffer, bitmap_size };
}

void WebContentView::schedule_repaint()
{
#ifdef LADYBIRD_QT_USE_VULKAN_WINDOW
    schedule_vulkan_window_update();
#else
    update();
#endif
}

#ifndef LADYBIRD_QT_USE_RHI_WIDGET
void WebContentView::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.scale(1 / m_device_pixel_ratio, 1 / m_device_pixel_ratio);

    auto paintable = current_paintable();
    Gfx::Bitmap const* bitmap = nullptr;
    Gfx::IntSize bitmap_size;
    if (paintable.has_value()) {
        bitmap = paintable->shared_image_buffer->bitmap().ptr();
        bitmap_size = paintable->bitmap_size;
    }

    if (bitmap) {
        QImage q_image(bitmap->scanline_u8(0), bitmap->width(), bitmap->height(), bitmap->pitch(), QImage::Format_RGB32);
        painter.drawImage(QPoint(0, 0), q_image, QRect(0, 0, bitmap_size.width(), bitmap_size.height()));

        auto background_color = page_background_color();
        auto fallback_color = QColor(background_color.red(), background_color.green(), background_color.blue());
        if (bitmap_size.width() < m_viewport_size.width()) {
            painter.fillRect(bitmap_size.width(), 0, m_viewport_size.width() - bitmap_size.width(), bitmap->height(), fallback_color);
        }
        if (bitmap_size.height() < m_viewport_size.height()) {
            painter.fillRect(0, bitmap_size.height(), m_viewport_size.width(), m_viewport_size.height() - bitmap_size.height(), fallback_color);
        }

        return;
    }

    auto background_color = page_background_color();
    painter.fillRect(QRect(0, 0, m_viewport_size.width(), m_viewport_size.height()), QColor(background_color.red(), background_color.green(), background_color.blue()));
}
#endif

Optional<QPixmap> WebContentView::tab_preview_pixmap(QSize const& maximum_size) const
{
    auto paintable = current_paintable();
    if (!paintable.has_value())
        return {};

    auto const* bitmap = paintable->shared_image_buffer->bitmap().ptr();
    if (!bitmap)
        return {};

    auto width = min(bitmap->width(), paintable->bitmap_size.width());
    auto height = min(bitmap->height(), paintable->bitmap_size.height());
    if (width <= 0 || height <= 0 || maximum_size.isEmpty())
        return {};

    QImage image(bitmap->scanline_u8(0), bitmap->width(), bitmap->height(), bitmap->pitch(), QImage::Format_RGB32);
    auto snapshot = image.copy(0, 0, width, height);
    if (snapshot.isNull())
        return {};

    auto preview_size = snapshot.size().scaled(maximum_size, Qt::KeepAspectRatio);
    if (preview_size.isEmpty())
        return {};

    auto preview = QPixmap::fromImage(snapshot).scaled(preview_size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (preview.isNull())
        return {};

    return preview;
}

void WebContentView::resizeEvent(QResizeEvent* event)
{
    WebContentViewBase::resizeEvent(event);
#ifdef LADYBIRD_QT_USE_VULKAN_WINDOW
    update_vulkan_window_geometry();
#endif
    update_viewport_size();
    handle_resize();
}

void WebContentView::set_viewport_rect(Gfx::IntRect rect)
{
    m_viewport_size = rect.size();
    handle_resize();
}

void WebContentView::set_device_pixel_ratio(double device_pixel_ratio)
{
    m_device_pixel_ratio = device_pixel_ratio;
    update_viewport_size();
    handle_resize();
}

void WebContentView::set_zoom_level(double zoom_level)
{
    m_zoom_level = zoom_level;
    client().async_set_zoom_level(m_client_state.page_index, m_zoom_level);
    update_zoom();
}

void WebContentView::set_maximum_frames_per_second(double maximum_frames_per_second)
{
    set_display_metadata(m_display_id, maximum_frames_per_second);
}

void WebContentView::set_display_metadata(Optional<u64> display_id, double maximum_frames_per_second)
{
    m_display_id = display_id;
    m_maximum_frames_per_second = maximum_frames_per_second;
    client().async_set_maximum_frames_per_second(m_client_state.page_index, m_maximum_frames_per_second);
    update_compositor_display_metadata();
}

void WebContentView::update_compositor_display_metadata()
{
    if (!m_client_state.client)
        return;

    auto compositor_context_id = client().compositor_context_id_for_page(m_client_state.page_index);
    WebView::Application::the().update_compositor_display_metadata(compositor_context_id, m_display_id, m_maximum_frames_per_second);
}

void WebContentView::update_viewport_size()
{
    auto scaled_width = int(width() * m_device_pixel_ratio);
    auto scaled_height = int(height() * m_device_pixel_ratio);
    Gfx::IntRect rect(0, 0, scaled_width, scaled_height);

    set_viewport_rect(rect);
}

void WebContentView::update_zoom()
{
    ViewImplementation::update_zoom();
    update_viewport_size();
}

void WebContentView::showEvent(QShowEvent* event)
{
    WebContentViewBase::showEvent(event);
    set_system_visibility_state(Web::HTML::VisibilityState::Visible);
}

void WebContentView::hideEvent(QHideEvent* event)
{
    WebContentViewBase::hideEvent(event);
    set_system_visibility_state(Web::HTML::VisibilityState::Hidden);
}

static Core::AnonymousBuffer make_system_theme_from_qt_palette(QWidget& widget, WebContentView::PaletteMode mode)
{
    auto qt_palette = widget.palette();

    auto theme_file = mode == WebContentView::PaletteMode::Default ? "Default"sv : "Dark"sv;
    auto theme_ini = MUST(Core::Resource::load_from_uri(MUST(String::formatted("resource://themes/{}.ini", theme_file))));
    auto theme = Gfx::load_system_theme(theme_ini->filesystem_path().to_byte_string()).release_value_but_fixme_should_propagate_errors();

    auto palette_impl = Gfx::PaletteImpl::create_with_anonymous_buffer(theme);
    auto palette = Gfx::Palette(move(palette_impl));

    auto translate = [&](Gfx::ColorRole gfx_color_role, QPalette::ColorRole qt_color_role) {
        auto new_color = Gfx::Color::from_bgra(qt_palette.color(qt_color_role).rgba());
        palette.set_color(gfx_color_role, new_color);
    };

    translate(Gfx::ColorRole::ThreedHighlight, QPalette::ColorRole::Light);
    translate(Gfx::ColorRole::ThreedShadow1, QPalette::ColorRole::Mid);
    translate(Gfx::ColorRole::ThreedShadow2, QPalette::ColorRole::Dark);
    translate(Gfx::ColorRole::HoverHighlight, QPalette::ColorRole::Light);
    translate(Gfx::ColorRole::Link, QPalette::ColorRole::Link);
    translate(Gfx::ColorRole::VisitedLink, QPalette::ColorRole::LinkVisited);
    translate(Gfx::ColorRole::Button, QPalette::ColorRole::Button);
    translate(Gfx::ColorRole::ButtonText, QPalette::ColorRole::ButtonText);
#ifdef AK_OS_MACOS
    palette.set_color(Gfx::ColorRole::Selection, WebView::macos_web_selection_color());
    palette.set_color(Gfx::ColorRole::InactiveSelection, appkit_web_inactive_selection_color());
    palette.set_color(Gfx::ColorRole::InactiveSelectionText, appkit_web_inactive_selection_text_color());
#else
    translate(Gfx::ColorRole::Selection, QPalette::ColorRole::Highlight);
#endif

    palette.set_flag(Gfx::FlagRole::IsDark, is_using_dark_system_theme(widget));

    return theme;
}

void WebContentView::update_palette(PaletteMode mode)
{
    set_page_background_color_to_system_canvas(is_using_dark_system_theme(*this));
    client().async_update_system_theme(m_client_state.page_index, make_system_theme_from_qt_palette(*this, mode));
}

void WebContentView::update_screen_rects()
{
    auto screens = QGuiApplication::screens();

    if (!screens.empty()) {
        Vector<Web::DevicePixelRect> screen_rects;
        for (auto const& screen : screens) {
            // NOTE: QScreen::geometry() returns the 'device-independent pixels', we multiply
            //       by the device pixel ratio to get the 'physical pixels' of the display.
            auto geometry = screen->geometry();
            auto device_pixel_ratio = screen->devicePixelRatio();
            screen_rects.append(Web::DevicePixelRect(geometry.x(), geometry.y(), geometry.width() * device_pixel_ratio, geometry.height() * device_pixel_ratio));
        }

        // NOTE: The first item in QGuiApplication::screens is always the primary screen.
        //       This is not specified in the documentation but QGuiApplication::primaryScreen
        //       always returns the first item in the list if it isn't empty.
        client().async_update_screen_rects(m_client_state.page_index, screen_rects, 0);
    }
}

void WebContentView::initialize_client(WebView::ViewImplementation::CreateNewClient create_new_client)
{
    ViewImplementation::initialize_client(create_new_client);

    update_compositor_display_metadata();
    update_palette();
    update_screen_rects();
}

void WebContentView::update_cursor(Gfx::Cursor cursor)
{
    cursor.visit([this](Gfx::StandardCursor standard_cursor) {
        switch (standard_cursor) {
        case Gfx::StandardCursor::Hidden:
            apply_web_content_cursor(Qt::BlankCursor);
            break;
        case Gfx::StandardCursor::Arrow:
            apply_web_content_cursor(Qt::ArrowCursor);
            break;
        case Gfx::StandardCursor::Crosshair:
            apply_web_content_cursor(Qt::CrossCursor);
            break;
        case Gfx::StandardCursor::IBeam:
            apply_web_content_cursor(Qt::IBeamCursor);
            break;
        case Gfx::StandardCursor::ResizeHorizontal:
            apply_web_content_cursor(Qt::SizeHorCursor);
            break;
        case Gfx::StandardCursor::ResizeVertical:
            apply_web_content_cursor(Qt::SizeVerCursor);
            break;
        case Gfx::StandardCursor::ResizeDiagonalTLBR:
            apply_web_content_cursor(Qt::SizeFDiagCursor);
            break;
        case Gfx::StandardCursor::ResizeDiagonalBLTR:
            apply_web_content_cursor(Qt::SizeBDiagCursor);
            break;
        case Gfx::StandardCursor::ResizeColumn:
            apply_web_content_cursor(Qt::SplitHCursor);
            break;
        case Gfx::StandardCursor::ResizeRow:
            apply_web_content_cursor(Qt::SplitVCursor);
            break;
        case Gfx::StandardCursor::Hand:
            apply_web_content_cursor(Qt::PointingHandCursor);
            break;
        case Gfx::StandardCursor::Help:
            apply_web_content_cursor(Qt::WhatsThisCursor);
            break;
        case Gfx::StandardCursor::OpenHand:
            apply_web_content_cursor(Qt::OpenHandCursor);
            break;
        case Gfx::StandardCursor::Drag:
            apply_web_content_cursor(Qt::ClosedHandCursor);
            break;
        case Gfx::StandardCursor::DragCopy:
            apply_web_content_cursor(Qt::DragCopyCursor);
            break;
        case Gfx::StandardCursor::Move:
            apply_web_content_cursor(Qt::DragMoveCursor);
            break;
        case Gfx::StandardCursor::Wait:
            apply_web_content_cursor(Qt::BusyCursor);
            break;
        case Gfx::StandardCursor::Disallowed:
            apply_web_content_cursor(Qt::ForbiddenCursor);
            break;
        case Gfx::StandardCursor::Eyedropper:
        case Gfx::StandardCursor::Zoom:
            // FIXME: No corresponding Qt cursors, default to Arrow
        default:
            apply_web_content_cursor(Qt::ArrowCursor);
            break;
        } },
        [this](Gfx::ImageCursor const& image_cursor) {
            if (!image_cursor.bitmap.is_valid()) {
                dbgln("Failed to set cursor: Bitmap is invalid.");
                return;
            }
            auto const& bitmap = *image_cursor.bitmap.bitmap();
            auto qimage = QImage { bitmap.scanline_u8(0), bitmap.width(), bitmap.height(), QImage::Format_ARGB32 };
            if (qimage.isNull()) {
                dbgln("Failed to set cursor: Null QImage.");
                return;
            }
            auto qpixmap = QPixmap::fromImage(qimage);
            if (qimage.isNull()) {
                dbgln("Failed to set cursor: Couldn't create QPixmap from QImage.");
                return;
            }
            apply_web_content_cursor(QCursor { qpixmap, image_cursor.hotspot.x(), image_cursor.hotspot.y() });
        });
}

void WebContentView::apply_web_content_cursor(QCursor const& cursor)
{
    setCursor(cursor);
#ifdef LADYBIRD_QT_USE_VULKAN_WINDOW
    set_vulkan_window_cursor(cursor);
#endif
}

Web::DevicePixelPoint WebContentView::node_picker_position_for(QSinglePointEvent const& event) const
{
    return { event.position().x() * m_device_pixel_ratio, event.position().y() * m_device_pixel_ratio };
}

Web::DevicePixelSize WebContentView::viewport_size() const
{
    return m_viewport_size.to_type<Web::DevicePixels>();
}

QPoint WebContentView::map_point_to_global_position(Gfx::IntPoint position) const
{
    return mapToGlobal(QPoint { position.x(), position.y() } / device_pixel_ratio());
}

Gfx::IntPoint WebContentView::to_content_position(Gfx::IntPoint widget_position) const
{
    return widget_position;
}

Gfx::IntPoint WebContentView::to_widget_position(Gfx::IntPoint content_position) const
{
    return content_position;
}

bool WebContentView::event(QEvent* event)
{
    // NOTE: We have to implement event() manually as Qt's focus navigation mechanism
    //       eats all the Tab key presses by default.

    if (event->type() == QEvent::KeyPress) {
        keyPressEvent(static_cast<QKeyEvent*>(event));
        return true;
    }
    if (event->type() == QEvent::KeyRelease) {
        keyReleaseEvent(static_cast<QKeyEvent*>(event));
        return true;
    }
    if (event->type() == QEvent::NativeGesture) {
        auto const& native_gesture_event = *static_cast<QNativeGestureEvent const*>(event);
        if (native_gesture_event.gestureType() == Qt::ZoomNativeGesture) {
            Web::PinchEvent pinch_event;
            auto const local_position = mapFromGlobal(native_gesture_event.globalPosition());
            pinch_event.position = { local_position.x() * m_device_pixel_ratio, local_position.y() * m_device_pixel_ratio };
            pinch_event.modifiers = get_modifiers_from_qt_keyboard_modifiers(native_gesture_event.modifiers());
            pinch_event.scale_delta = native_gesture_event.value();
            enqueue_input_event(AK::move(pinch_event));
            return true;
        }
    }

    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::ThemeChange) {
        QTimer::singleShot(0, this, [this] {
            update_palette();
            schedule_repaint();
        });
        return WebContentViewBase::event(event);
    }

    if (event->type() == QEvent::ShortcutOverride) {
        auto* key_event = static_cast<QKeyEvent*>(event);
        if (is_browser_reserved_shortcut(*key_event)) {
            event->ignore();
            return false;
        }

        event->accept();
        return true;
    }

    return WebContentViewBase::event(event);
}

#ifdef LADYBIRD_QT_USE_VULKAN_WINDOW
bool WebContentView::handle_vulkan_window_event(QEvent* event)
{
    switch (event->type()) {
    case QEvent::KeyPress:
        keyPressEvent(static_cast<QKeyEvent*>(event));
        return true;
    case QEvent::KeyRelease:
        keyReleaseEvent(static_cast<QKeyEvent*>(event));
        return true;
    case QEvent::MouseMove:
        mouseMoveEvent(static_cast<QMouseEvent*>(event));
        emit native_window_pointer_event();
        return true;
    case QEvent::MouseButtonPress:
        mousePressEvent(static_cast<QMouseEvent*>(event));
        return true;
    case QEvent::MouseButtonRelease:
        mouseReleaseEvent(static_cast<QMouseEvent*>(event));
        return true;
    case QEvent::MouseButtonDblClick:
        mouseDoubleClickEvent(static_cast<QMouseEvent*>(event));
        return true;
    case QEvent::Wheel:
        wheelEvent(static_cast<QWheelEvent*>(event));
        return true;
    case QEvent::Leave:
        leaveEvent(event);
        emit native_window_pointer_event();
        return true;
    case QEvent::FocusIn:
        focusInEvent(static_cast<QFocusEvent*>(event));
        return true;
    case QEvent::FocusOut:
        focusOutEvent(static_cast<QFocusEvent*>(event));
        return true;
    case QEvent::InputMethod:
        inputMethodEvent(static_cast<QInputMethodEvent*>(event));
        return true;
    case QEvent::DragEnter:
        dragEnterEvent(static_cast<QDragEnterEvent*>(event));
        return true;
    case QEvent::DragMove:
        dragMoveEvent(static_cast<QDragMoveEvent*>(event));
        return true;
    case QEvent::DragLeave:
        dragLeaveEvent(static_cast<QDragLeaveEvent*>(event));
        return true;
    case QEvent::Drop:
        dropEvent(static_cast<QDropEvent*>(event));
        return true;
    case QEvent::NativeGesture: {
        auto const& native_gesture_event = *static_cast<QNativeGestureEvent const*>(event);
        if (native_gesture_event.gestureType() == Qt::ZoomNativeGesture) {
            Web::PinchEvent pinch_event;
            auto const local_position = mapFromGlobal(native_gesture_event.globalPosition());
            pinch_event.position = { local_position.x() * m_device_pixel_ratio, local_position.y() * m_device_pixel_ratio };
            pinch_event.modifiers = get_modifiers_from_qt_keyboard_modifiers(native_gesture_event.modifiers());
            pinch_event.scale_delta = native_gesture_event.value();
            enqueue_input_event(AK::move(pinch_event));
            return true;
        }
        return false;
    }
    case QEvent::ShortcutOverride: {
        auto* key_event = static_cast<QKeyEvent*>(event);
        if (is_browser_reserved_shortcut(*key_event)) {
            event->ignore();
            return false;
        }

        event->accept();
        return true;
    }
    default:
        return false;
    }
}
#endif

void WebContentView::enqueue_native_event(Web::MouseEvent::Type type, QSinglePointEvent const& event)
{
    Web::DevicePixelPoint position = { event.position().x() * m_device_pixel_ratio, event.position().y() * m_device_pixel_ratio };
    auto screen_position = Gfx::IntPoint { event.globalPosition().x() * m_device_pixel_ratio, event.globalPosition().y() * m_device_pixel_ratio };

    auto button = get_button_from_qt_mouse_button(event.button());
    auto buttons = get_buttons_from_qt_mouse_buttons(event.buttons());
    auto modifiers = get_modifiers_from_qt_keyboard_modifiers(event.modifiers());

    if (button == 0 && (type == Web::MouseEvent::Type::MouseDown || type == Web::MouseEvent::Type::MouseUp)) {
        // We could not convert Qt buttons to something that LibWeb can recognize - don't even bother propagating this
        // to the web engine as it will not handle it anyway, and it will (currently) assert.
        return;
    }

    double wheel_delta_x = 0;
    double wheel_delta_y = 0;

    if (type == Web::MouseEvent::Type::MouseWheel) {
        auto const& wheel_event = static_cast<QWheelEvent const&>(event);

        if (auto pixel_delta = -wheel_event.pixelDelta(); !pixel_delta.isNull()) {
            wheel_delta_x = pixel_delta.x();
            wheel_delta_y = pixel_delta.y();
        } else {
            auto angle_delta = -wheel_event.angleDelta();
            double delta_x = -static_cast<double>(angle_delta.x()) / 120.0;
            double delta_y = static_cast<double>(angle_delta.y()) / 120.0;

            static constexpr double scroll_step_size = 40;
            auto step_x = delta_x * static_cast<double>(QApplication::wheelScrollLines());
            auto step_y = delta_y * static_cast<double>(QApplication::wheelScrollLines());

            wheel_delta_x = step_x * scroll_step_size;
            wheel_delta_y = step_y * scroll_step_size;
        }
    }

    enqueue_input_event(Web::MouseEvent { type, position, screen_position.to_type<Web::DevicePixels>(), button, buttons, modifiers, wheel_delta_x, wheel_delta_y, m_click_count, nullptr });
}

struct DragData : Web::BrowserInputData {
    explicit DragData(QDropEvent const& event)
        : urls(event.mimeData()->urls())
    {
    }

    QList<QUrl> urls;
};

void WebContentView::enqueue_native_event(Web::DragEvent::Type type, QDropEvent const& event)
{
    Web::DevicePixelPoint position = { event.position().x() * m_device_pixel_ratio, event.position().y() * m_device_pixel_ratio };

    auto global_position = mapToGlobal(event.position());
    auto screen_position = Gfx::IntPoint { global_position.x() * m_device_pixel_ratio, global_position.y() * m_device_pixel_ratio };

    auto button = get_button_from_qt_mouse_button(Qt::LeftButton);
    auto buttons = get_buttons_from_qt_mouse_buttons(event.buttons());
    auto modifiers = get_modifiers_from_qt_keyboard_modifiers(event.modifiers());

    Vector<Web::HTML::SelectedFile> files;
    OwnPtr<DragData> browser_data;

    if (type == Web::DragEvent::Type::DragStart) {
        VERIFY(event.mimeData()->hasUrls());

        for (auto const& url : event.mimeData()->urls()) {
            auto file_path = ak_byte_string_from_qstring(url.toLocalFile());

            if (auto file = WebView::create_selected_file(file_path); file.is_error())
                warnln("Unable to open file {}: {}", file_path, file.error());
            else
                files.append(file.release_value());
        }
    } else if (type == Web::DragEvent::Type::Drop) {
        browser_data = make<DragData>(event);
    }

    enqueue_input_event(Web::DragEvent { type, position, screen_position.to_type<Web::DevicePixels>(), button, buttons, modifiers, AK::move(files), AK::move(browser_data) });
}

void WebContentView::finish_handling_drag_event(Web::DragEvent const& event)
{
    if (event.type != Web::DragEvent::Type::Drop)
        return;

    auto const& browser_data = as<DragData>(*event.browser_data);
    emit urls_dropped(browser_data.urls);
}

struct KeyData : Web::BrowserInputData {
    explicit KeyData(QKeyEvent const& event)
        : event(adopt_own(*event.clone()))
    {
    }

    NonnullOwnPtr<QKeyEvent> event;
};

void WebContentView::enqueue_native_event(Web::KeyEvent::Type type, QKeyEvent const& event)
{
    auto keycode = get_keycode_from_qt_key_event(event);
    auto modifiers = get_modifiers_from_qt_key_event(event);

    auto text = event.text();
    auto code_point = text.isEmpty() ? 0u : event.text()[0].unicode();
    auto should_insert_text = type == Web::KeyEvent::Type::KeyDown && !text.isEmpty();

    auto to_web_event = [&]() -> Web::KeyEvent {
        if (event.key() == Qt::Key_Backtab) {
            // Qt transforms Shift+Tab into a "Backtab", so we undo that transformation here.
            return { type, Web::UIEvents::KeyCode::Key_Tab, Web::UIEvents::Mod_Shift, '\t', event.isAutoRepeat(), false, make<KeyData>(event) };
        }

        if (event.key() == Qt::Key_Enter || event.key() == Qt::Key_Return) {
            // This ensures consistent behavior between systems that treat Enter as '\n' and '\r\n'
            return { type, Web::UIEvents::KeyCode::Key_Return, modifiers, '\n', event.isAutoRepeat(), should_insert_text, make<KeyData>(event) };
        }

        return { type, keycode, modifiers, code_point, event.isAutoRepeat(), should_insert_text, make<KeyData>(event) };
    };

    enqueue_input_event(to_web_event());
}

void WebContentView::finish_handling_key_event(Web::KeyEvent const& key_event)
{
    auto& browser_data = as<KeyData>(*key_event.browser_data);
    auto& event = *browser_data.event;

    switch (key_event.type) {
    case Web::KeyEvent::Type::KeyDown:
        WebContentViewBase::keyPressEvent(&event);
        break;
    case Web::KeyEvent::Type::KeyUp:
        WebContentViewBase::keyReleaseEvent(&event);
        break;
    }

    if (!event.isAccepted())
        QApplication::sendEvent(parent(), &event);
}

}
