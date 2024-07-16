/*
 * Copyright (c) 2022-2023, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "WebContentView.h"
#include "Application.h"
#include "StringUtils.h"
#include <AK/Assertions.h>
#include <AK/ByteBuffer.h>
#include <AK/Format.h>
#include <AK/LexicalPath.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Types.h>
#include <Ladybird/HelperProcess.h>
#include <Ladybird/Utilities.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Resource.h>
#include <LibCore/Timer.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibGfx/Painter.h>
#include <LibGfx/Palette.h>
#include <LibGfx/Rect.h>
#include <LibGfx/SystemTheme.h>
#include <LibWeb/Crypto/Crypto.h>
#include <LibWeb/UIEvents/KeyCode.h>
#include <LibWeb/UIEvents/MouseButton.h>
#include <LibWeb/Worker/WebWorkerClient.h>
#include <LibWebView/WebContentClient.h>
#include <QApplication>
#include <QCursor>
#include <QGuiApplication>
#include <QIcon>
#include <QLineEdit>
#include <QMimeData>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPalette>
#include <QScrollBar>
#include <QTextEdit>
#include <QTimer>
#include <QToolTip>

namespace Ladybird {

bool is_using_dark_system_theme(QWidget&);

WebContentView::WebContentView(QWidget* window, WebContentOptions const& web_content_options, StringView webdriver_content_ipc_path, RefPtr<WebView::WebContentClient> parent_client, size_t page_index)
    : QAbstractScrollArea(window)
    , m_web_content_options(web_content_options)
    , m_webdriver_content_ipc_path(webdriver_content_ipc_path)
{
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_client_state.client = parent_client;
    m_client_state.page_index = page_index;

    setMouseTracking(true);
    setAcceptDrops(true);

    setFocusPolicy(Qt::FocusPolicy::StrongFocus);

    m_device_pixel_ratio = devicePixelRatio();

    verticalScrollBar()->setSingleStep(24);
    horizontalScrollBar()->setSingleStep(24);

    QObject::connect(qGuiApp, &QGuiApplication::screenRemoved, [this](QScreen*) {
        update_screen_rects();
    });

    QObject::connect(qGuiApp, &QGuiApplication::screenAdded, [this](QScreen*) {
        update_screen_rects();
    });

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
        viewport()->update();
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

    on_request_worker_agent = [&]() {
        RefPtr<Protocol::RequestClient> request_server_client {};
        if (m_web_content_options.use_lagom_networking == Ladybird::UseLagomNetworking::Yes)
            request_server_client = static_cast<Ladybird::Application*>(QApplication::instance())->request_server_client;

        auto worker_client = MUST(launch_web_worker_process(MUST(get_paths_for_helper_process("WebWorker"sv)), request_server_client));
        return worker_client->dup_socket();
    };
}

WebContentView::~WebContentView()
{
    if (m_client_state.client)
        m_client_state.client->unregister_view(m_client_state.page_index);
}

static Web::UIEvents::MouseButton get_button_from_qt_event(QSinglePointEvent const& event)
{
    if (event.button() == Qt::MouseButton::LeftButton)
        return Web::UIEvents::MouseButton::Primary;
    if (event.button() == Qt::MouseButton::RightButton)
        return Web::UIEvents::MouseButton::Secondary;
    if (event.button() == Qt::MouseButton::MiddleButton)
        return Web::UIEvents::MouseButton::Middle;
    if (event.button() == Qt::MouseButton::BackButton)
        return Web::UIEvents::MouseButton::Backward;
    if (event.buttons() == Qt::MouseButton::ForwardButton)
        return Web::UIEvents::MouseButton::Forward;
    return Web::UIEvents::MouseButton::None;
}

static Web::UIEvents::MouseButton get_buttons_from_qt_event(QSinglePointEvent const& event)
{
    auto buttons = Web::UIEvents::MouseButton::None;
    if (event.buttons().testFlag(Qt::MouseButton::LeftButton))
        buttons |= Web::UIEvents::MouseButton::Primary;
    if (event.buttons().testFlag(Qt::MouseButton::RightButton))
        buttons |= Web::UIEvents::MouseButton::Secondary;
    if (event.buttons().testFlag(Qt::MouseButton::MiddleButton))
        buttons |= Web::UIEvents::MouseButton::Middle;
    if (event.buttons().testFlag(Qt::MouseButton::BackButton))
        buttons |= Web::UIEvents::MouseButton::Backward;
    if (event.buttons().testFlag(Qt::MouseButton::ForwardButton))
        buttons |= Web::UIEvents::MouseButton::Forward;
    return buttons;
}

static Web::UIEvents::KeyModifier get_modifiers_from_qt_mouse_event(QSinglePointEvent const& event)
{
    auto modifiers = Web::UIEvents::KeyModifier::Mod_None;
    if (event.modifiers().testFlag(Qt::AltModifier))
        modifiers |= Web::UIEvents::KeyModifier::Mod_Alt;
    if (event.modifiers().testFlag(Qt::ControlModifier))
        modifiers |= Web::UIEvents::KeyModifier::Mod_Ctrl;
    if (event.modifiers().testFlag(Qt::ShiftModifier))
        modifiers |= Web::UIEvents::KeyModifier::Mod_Shift;
    return modifiers;
}

static Web::UIEvents::KeyModifier get_modifiers_from_qt_keyboard_event(QKeyEvent const& event)
{
    auto modifiers = Web::UIEvents::KeyModifier::Mod_None;
    if (event.modifiers().testFlag(Qt::AltModifier))
        modifiers |= Web::UIEvents::KeyModifier::Mod_Alt;
    if (event.modifiers().testFlag(Qt::ControlModifier))
        modifiers |= Web::UIEvents::KeyModifier::Mod_Ctrl;
    if (event.modifiers().testFlag(Qt::MetaModifier))
        modifiers |= Web::UIEvents::KeyModifier::Mod_Super;
    if (event.modifiers().testFlag(Qt::ShiftModifier))
        modifiers |= Web::UIEvents::KeyModifier::Mod_Shift;
    if (event.modifiers().testFlag(Qt::AltModifier))
        modifiers |= Web::UIEvents::KeyModifier::Mod_AltGr;
    if (event.modifiers().testFlag(Qt::KeypadModifier))
        modifiers |= Web::UIEvents::KeyModifier::Mod_Keypad;
    return modifiers;
}

static Web::UIEvents::KeyCode get_keycode_from_qt_keyboard_event(QKeyEvent const& event)
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

    // https://doc.qt.io/qt-6/qt.html#Key-enum
    constexpr Mapping mappings[] = {
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
        { Qt::Key_Alt, Web::UIEvents::Key_Alt },
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
        { Qt::Key_Control, Web::UIEvents::Key_Control },
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
        { Qt::Key_Meta, Web::UIEvents::Key_Super },
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
        { Qt::Key_Super_L, Web::UIEvents::Key_Super },
        { Qt::Key_Super_R, Web::UIEvents::Key_Super },
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

void WebContentView::keyPressEvent(QKeyEvent* event)
{
    enqueue_native_event(Web::KeyEvent::Type::KeyDown, *event);
}

void WebContentView::keyReleaseEvent(QKeyEvent* event)
{
    enqueue_native_event(Web::KeyEvent::Type::KeyUp, *event);
}

void WebContentView::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_tooltip_override) {
        if (QToolTip::isVisible())
            QToolTip::hideText();
        m_tooltip_hover_timer.start(600);
    }

    enqueue_native_event(Web::MouseEvent::Type::MouseMove, *event);
}

void WebContentView::mousePressEvent(QMouseEvent* event)
{
    enqueue_native_event(Web::MouseEvent::Type::MouseDown, *event);
}

void WebContentView::mouseReleaseEvent(QMouseEvent* event)
{
    enqueue_native_event(Web::MouseEvent::Type::MouseUp, *event);

    if (event->button() == Qt::MouseButton::BackButton) {
        if (on_navigate_back)
            on_navigate_back();
    } else if (event->button() == Qt::MouseButton::ForwardButton) {
        if (on_navigate_forward)
            on_navigate_forward();
    }
}

void WebContentView::wheelEvent(QWheelEvent* event)
{
    if (event->modifiers().testFlag(Qt::ControlModifier)) {
        event->ignore();
        return;
    }

    enqueue_native_event(Web::MouseEvent::Type::MouseWheel, *event);
}

void WebContentView::mouseDoubleClickEvent(QMouseEvent* event)
{
    enqueue_native_event(Web::MouseEvent::Type::DoubleClick, *event);
}

void WebContentView::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void WebContentView::dropEvent(QDropEvent* event)
{
    VERIFY(event->mimeData()->hasUrls());
    emit urls_dropped(event->mimeData()->urls());
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

void WebContentView::paintEvent(QPaintEvent*)
{
    QPainter painter(viewport());
    painter.scale(1 / m_device_pixel_ratio, 1 / m_device_pixel_ratio);

    Gfx::Bitmap const* bitmap = nullptr;
    Gfx::IntSize bitmap_size;

    if (m_client_state.has_usable_bitmap) {
        bitmap = m_client_state.front_bitmap.bitmap.ptr();
        bitmap_size = m_client_state.front_bitmap.last_painted_size.to_type<int>();

    } else {
        bitmap = m_backup_bitmap.ptr();
        bitmap_size = m_backup_bitmap_size.to_type<int>();
    }

    if (bitmap) {
        QImage q_image(bitmap->scanline_u8(0), bitmap->width(), bitmap->height(), bitmap->pitch(), QImage::Format_RGB32);
        painter.drawImage(QPoint(0, 0), q_image, QRect(0, 0, bitmap_size.width(), bitmap_size.height()));

        if (bitmap_size.width() < width()) {
            painter.fillRect(bitmap_size.width(), 0, width() - bitmap_size.width(), bitmap->height(), palette().base());
        }
        if (bitmap_size.height() < height()) {
            painter.fillRect(0, bitmap_size.height(), width(), height() - bitmap_size.height(), palette().base());
        }

        return;
    }

    painter.fillRect(rect(), palette().base());
}

void WebContentView::resizeEvent(QResizeEvent* event)
{
    QAbstractScrollArea::resizeEvent(event);
    update_viewport_size();
    handle_resize();
}

void WebContentView::set_viewport_rect(Gfx::IntRect rect)
{
    m_viewport_size = rect.size();
    client().async_set_viewport_size(m_client_state.page_index, rect.size().to_type<Web::DevicePixels>());
}

void WebContentView::set_window_size(Gfx::IntSize size)
{
    client().async_set_window_size(m_client_state.page_index, size.to_type<Web::DevicePixels>());
}

void WebContentView::set_window_position(Gfx::IntPoint position)
{
    client().async_set_window_position(m_client_state.page_index, position.to_type<Web::DevicePixels>());
}

void WebContentView::set_device_pixel_ratio(double device_pixel_ratio)
{
    m_device_pixel_ratio = device_pixel_ratio;
    client().async_set_device_pixels_per_css_pixel(m_client_state.page_index, m_device_pixel_ratio * m_zoom_level);
    update_viewport_size();
    handle_resize();
}

void WebContentView::update_viewport_size()
{
    auto scaled_width = int(viewport()->width() * m_device_pixel_ratio);
    auto scaled_height = int(viewport()->height() * m_device_pixel_ratio);
    Gfx::IntRect rect(0, 0, scaled_width, scaled_height);

    set_viewport_rect(rect);
}

void WebContentView::update_zoom()
{
    client().async_set_device_pixels_per_css_pixel(m_client_state.page_index, m_device_pixel_ratio * m_zoom_level);
    update_viewport_size();
}

void WebContentView::showEvent(QShowEvent* event)
{
    QAbstractScrollArea::showEvent(event);
    client().async_set_system_visibility_state(m_client_state.page_index, true);
}

void WebContentView::hideEvent(QHideEvent* event)
{
    QAbstractScrollArea::hideEvent(event);
    client().async_set_system_visibility_state(m_client_state.page_index, false);
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
        auto new_color = Gfx::Color::from_argb(qt_palette.color(qt_color_role).rgba());
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
    translate(Gfx::ColorRole::Selection, QPalette::ColorRole::Highlight);
    translate(Gfx::ColorRole::SelectionText, QPalette::ColorRole::HighlightedText);

    palette.set_flag(Gfx::FlagRole::IsDark, is_using_dark_system_theme(widget));

    return theme;
}

void WebContentView::update_palette(PaletteMode mode)
{
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
    if (create_new_client == CreateNewClient::Yes) {
        m_client_state = {};

        Optional<IPC::File> request_server_socket;
        if (m_web_content_options.use_lagom_networking == UseLagomNetworking::Yes) {
            auto& protocol = static_cast<Ladybird::Application*>(QApplication::instance())->request_server_client;

            // FIXME: Fail to open the tab, rather than crashing the whole application if this fails
            auto socket = connect_new_request_server_client(*protocol).release_value_but_fixme_should_propagate_errors();
            request_server_socket = AK::move(socket);
        }

        auto image_decoder = static_cast<Ladybird::Application*>(QApplication::instance())->image_decoder_client();
        auto image_decoder_socket = connect_new_image_decoder_client(*image_decoder).release_value_but_fixme_should_propagate_errors();

        auto candidate_web_content_paths = get_paths_for_helper_process("WebContent"sv).release_value_but_fixme_should_propagate_errors();
        auto new_client = launch_web_content_process(*this, candidate_web_content_paths, m_web_content_options, AK::move(image_decoder_socket), AK::move(request_server_socket)).release_value_but_fixme_should_propagate_errors();

        m_client_state.client = new_client;
    } else {
        m_client_state.client->register_view(m_client_state.page_index, *this);
    }

    m_client_state.client->on_web_content_process_crash = [this] {
        Core::deferred_invoke([this] {
            handle_web_content_process_crash();
        });
    };

    m_client_state.client_handle = Web::Crypto::generate_random_uuid().release_value_but_fixme_should_propagate_errors();
    client().async_set_window_handle(m_client_state.page_index, m_client_state.client_handle);

    client().async_set_device_pixels_per_css_pixel(m_client_state.page_index, m_device_pixel_ratio);
    update_palette();

    update_screen_rects();

    if (!m_webdriver_content_ipc_path.is_empty())
        client().async_connect_to_webdriver(m_client_state.page_index, m_webdriver_content_ipc_path);
}

void WebContentView::update_cursor(Gfx::StandardCursor cursor)
{
    switch (cursor) {
    case Gfx::StandardCursor::Hidden:
        setCursor(Qt::BlankCursor);
        break;
    case Gfx::StandardCursor::Arrow:
        setCursor(Qt::ArrowCursor);
        break;
    case Gfx::StandardCursor::Crosshair:
        setCursor(Qt::CrossCursor);
        break;
    case Gfx::StandardCursor::IBeam:
        setCursor(Qt::IBeamCursor);
        break;
    case Gfx::StandardCursor::ResizeHorizontal:
        setCursor(Qt::SizeHorCursor);
        break;
    case Gfx::StandardCursor::ResizeVertical:
        setCursor(Qt::SizeVerCursor);
        break;
    case Gfx::StandardCursor::ResizeDiagonalTLBR:
        setCursor(Qt::SizeFDiagCursor);
        break;
    case Gfx::StandardCursor::ResizeDiagonalBLTR:
        setCursor(Qt::SizeBDiagCursor);
        break;
    case Gfx::StandardCursor::ResizeColumn:
        setCursor(Qt::SplitHCursor);
        break;
    case Gfx::StandardCursor::ResizeRow:
        setCursor(Qt::SplitVCursor);
        break;
    case Gfx::StandardCursor::Hand:
        setCursor(Qt::PointingHandCursor);
        break;
    case Gfx::StandardCursor::Help:
        setCursor(Qt::WhatsThisCursor);
        break;
    case Gfx::StandardCursor::Drag:
        setCursor(Qt::ClosedHandCursor);
        break;
    case Gfx::StandardCursor::DragCopy:
        setCursor(Qt::DragCopyCursor);
        break;
    case Gfx::StandardCursor::Move:
        setCursor(Qt::DragMoveCursor);
        break;
    case Gfx::StandardCursor::Wait:
        setCursor(Qt::BusyCursor);
        break;
    case Gfx::StandardCursor::Disallowed:
        setCursor(Qt::ForbiddenCursor);
        break;
    case Gfx::StandardCursor::Eyedropper:
    case Gfx::StandardCursor::Zoom:
        // FIXME: No corresponding Qt cursors, default to Arrow
    default:
        setCursor(Qt::ArrowCursor);
        break;
    }
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

    if (event->type() == QEvent::PaletteChange) {
        update_palette();
        return QAbstractScrollArea::event(event);
    }

    if (event->type() == QEvent::ShortcutOverride) {
        event->accept();
        return true;
    }

    return QAbstractScrollArea::event(event);
}

ErrorOr<String> WebContentView::dump_layout_tree()
{
    return String::from_byte_string(client().dump_layout_tree(m_client_state.page_index));
}

void WebContentView::enqueue_native_event(Web::MouseEvent::Type type, QSinglePointEvent const& event)
{
    Web::DevicePixelPoint position = { event.position().x() * m_device_pixel_ratio, event.position().y() * m_device_pixel_ratio };
    auto screen_position = Gfx::IntPoint { event.globalPosition().x() * m_device_pixel_ratio, event.globalPosition().y() * m_device_pixel_ratio };

    auto button = get_button_from_qt_event(event);
    auto buttons = get_buttons_from_qt_event(event);
    auto modifiers = get_modifiers_from_qt_mouse_event(event);

    if (button == 0 && (type == Web::MouseEvent::Type::MouseDown || type == Web::MouseEvent::Type::MouseUp)) {
        // We could not convert Qt buttons to something that LibWeb can recognize - don't even bother propagating this
        // to the web engine as it will not handle it anyway, and it will (currently) assert.
        return;
    }

    int wheel_delta_x = 0;
    int wheel_delta_y = 0;

    if (type == Web::MouseEvent::Type::MouseWheel) {
        auto const& wheel_event = static_cast<QWheelEvent const&>(event);

        if (auto pixel_delta = -wheel_event.pixelDelta(); !pixel_delta.isNull()) {
            wheel_delta_x = pixel_delta.x();
            wheel_delta_y = pixel_delta.y();
        } else {
            auto angle_delta = -wheel_event.angleDelta();
            float delta_x = -static_cast<float>(angle_delta.x()) / 120.0f;
            float delta_y = static_cast<float>(angle_delta.y()) / 120.0f;

            auto step_x = delta_x * static_cast<float>(QApplication::wheelScrollLines()) * m_device_pixel_ratio;
            auto step_y = delta_y * static_cast<float>(QApplication::wheelScrollLines()) * m_device_pixel_ratio;
            auto scroll_step_size = static_cast<float>(verticalScrollBar()->singleStep());

            wheel_delta_x = static_cast<int>(step_x * scroll_step_size);
            wheel_delta_y = static_cast<int>(step_y * scroll_step_size);
        }
    }

    enqueue_input_event(Web::MouseEvent { type, position, screen_position.to_type<Web::DevicePixels>(), button, buttons, modifiers, wheel_delta_x, wheel_delta_y, nullptr });
}

struct KeyData : Web::ChromeInputData {
    explicit KeyData(QKeyEvent const& event)
        : event(adopt_own(*event.clone()))
    {
    }

    NonnullOwnPtr<QKeyEvent> event;
};

void WebContentView::enqueue_native_event(Web::KeyEvent::Type type, QKeyEvent const& event)
{
    auto keycode = get_keycode_from_qt_keyboard_event(event);
    auto modifiers = get_modifiers_from_qt_keyboard_event(event);

    auto text = event.text();
    auto code_point = text.isEmpty() ? 0u : event.text()[0].unicode();

    auto to_web_event = [&]() -> Web::KeyEvent {
        if (event.key() == Qt::Key_Backtab) {
            // Qt transforms Shift+Tab into a "Backtab", so we undo that transformation here.
            return { type, Web::UIEvents::KeyCode::Key_Tab, Web::UIEvents::Mod_Shift, '\t', make<KeyData>(event) };
        }

        if (event.key() == Qt::Key_Enter || event.key() == Qt::Key_Return) {
            // This ensures consistent behavior between systems that treat Enter as '\n' and '\r\n'
            return { type, Web::UIEvents::KeyCode::Key_Return, Web::UIEvents::Mod_Shift, '\n', make<KeyData>(event) };
        }

        return { type, keycode, modifiers, code_point, make<KeyData>(event) };
    };

    enqueue_input_event(to_web_event());
}

void WebContentView::finish_handling_key_event(Web::KeyEvent const& key_event)
{
    auto& chrome_data = verify_cast<KeyData>(*key_event.chrome_data);
    auto& event = *chrome_data.event;

    switch (key_event.type) {
    case Web::KeyEvent::Type::KeyDown:
        QAbstractScrollArea::keyPressEvent(&event);
        break;
    case Web::KeyEvent::Type::KeyUp:
        QAbstractScrollArea::keyReleaseEvent(&event);
        break;
    }

    if (!event.isAccepted())
        QApplication::sendEvent(parent(), &event);
}

}
