/*
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringView.h>
#include <LibURL/URL.h>
#include <LibWebView/Application.h>
#include <LibWebView/URL.h>
#include <UI/Qt/Autocomplete.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/LocationEdit.h>
#include <UI/Qt/StringUtils.h>

#include <QAction>
#include <QApplication>
#include <QEasingCurve>
#include <QGraphicsDropShadowEffect>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QResizeEvent>
#include <QStyle>
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
#    include <QStyleHints>
#endif
#include <QTextLayout>
#include <QTimer>
#include <QToolButton>
#include <QVariantAnimation>

namespace Ladybird {

class LocationActionButton final : public QToolButton {
public:
    explicit LocationActionButton(QWidget* parent)
        : QToolButton(parent)
    {
    }

private:
    virtual void paintEvent(QPaintEvent*) override
    {
        static constexpr int hover_size = 23;
        static constexpr int icon_y_offset = -1;
        static constexpr qreal hover_y_offset = 1.0;

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        if (isDown() || underMouse()) {
            auto background = isDown()
                ? ChromeStyle::mix(ChromeStyle::chrome_surface_pressed(palette()), ChromeStyle::chrome_button_text(palette()), 0.04)
                : ChromeStyle::mix(ChromeStyle::chrome_surface_hover(palette()), ChromeStyle::chrome_button_text(palette()), 0.04);
            painter.setPen(Qt::NoPen);
            painter.setBrush(background);
            auto hover_rect = QRectF(0, 0, hover_size, hover_size);
            hover_rect.moveCenter(QPointF(rect().center().x(), rect().center().y() + hover_y_offset));
            painter.drawRoundedRect(hover_rect, 10, 10);
        }

        auto icon_rect = QRect(
            (width() - iconSize().width()) / 2,
            (height() - iconSize().height()) / 2 + icon_y_offset,
            iconSize().width(),
            iconSize().height());
        icon().paint(&painter, icon_rect, Qt::AlignCenter, isEnabled() ? QIcon::Normal : QIcon::Disabled, isDown() ? QIcon::On : QIcon::Off);
    }
};

static QColor location_focus_glow_color(QPalette const& palette, int alpha)
{
    auto color = ChromeStyle::chrome_accent(palette);
    color.setAlpha(alpha);
    return color;
}

static int qstring_offset_for_byte_offset(String const& text, size_t byte_offset)
{
    auto prefix = text.bytes_as_string_view().substring_view(0, byte_offset);
    return static_cast<int>(qstring_from_ak_string(prefix).length());
}

static constexpr int LOCATION_TRAILING_EDGE_MARGIN = 12;
static constexpr int LOCATION_TRAILING_TEXT_GAP = 4;
static constexpr int LOCATION_TRAILING_ITEM_GAP = 6;
static constexpr int LOCATION_TRAILING_ACTION_WIDTH = 24;
static constexpr int LOCATION_TRAILING_ACTION_HEIGHT = 23;
static constexpr int LOCATION_PILL_HEIGHT = 22;
static constexpr int LOCATION_PILL_HORIZONTAL_PADDING = 18;

LocationEdit::LocationEdit(QWidget* parent)
    : QLineEdit(parent)
    , m_autocomplete(new Autocomplete(this))
{
    setObjectName("LadybirdLocationEdit");
    setMinimumHeight(32);
    update_chrome_style();

    m_focus_glow_effect = new QGraphicsDropShadowEffect(this);
    m_focus_glow_effect->setBlurRadius(10);
    m_focus_glow_effect->setOffset(0, 0);
    update_focus_glow(0);
    setGraphicsEffect(m_focus_glow_effect);

    m_focus_glow_animation = new QVariantAnimation(this);
    m_focus_glow_animation->setDuration(130);
    m_focus_glow_animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_focus_glow_animation, &QVariantAnimation::valueChanged, this, [this](QVariant const& value) {
        update_focus_glow(value.toInt());
    });

    m_leading_icon_button = new QToolButton(this);
    m_leading_icon_button->setObjectName("LadybirdLocationIcon");
    m_leading_icon_button->setIconSize({ 18, 18 });
    m_leading_icon_button->setFixedSize(22, 22);
    m_leading_icon_button->setAutoRaise(true);
    m_leading_icon_button->setFocusPolicy(Qt::NoFocus);
    m_leading_icon_button->setCursor(Qt::ArrowCursor);
    m_leading_icon_button->hide();

    m_trailing_action_button = new LocationActionButton(this);
    m_trailing_action_button->setObjectName("LadybirdLocationAction");
    m_trailing_action_button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_trailing_action_button->setIconSize({ 17, 17 });
    m_trailing_action_button->setFixedSize(LOCATION_TRAILING_ACTION_WIDTH, LOCATION_TRAILING_ACTION_HEIGHT);
    m_trailing_action_button->setAutoRaise(true);
    m_trailing_action_button->setFocusPolicy(Qt::NoFocus);
    m_trailing_action_button->setCursor(Qt::ArrowCursor);
    m_trailing_action_button->hide();

    m_zoom_indicator_button = new QToolButton(this);
    m_zoom_indicator_button->setObjectName("LadybirdLocationZoomIndicator");
    m_zoom_indicator_button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_zoom_indicator_button->setFixedHeight(LOCATION_PILL_HEIGHT);
    m_zoom_indicator_button->setAutoRaise(true);
    m_zoom_indicator_button->setFocusPolicy(Qt::NoFocus);
    m_zoom_indicator_button->setCursor(Qt::ArrowCursor);
    m_zoom_indicator_button->hide();
    connect(m_zoom_indicator_button, &QToolButton::clicked, this, [this] {
        if (m_zoom_action)
            m_zoom_action->trigger();
    });

    update_text_margins();
    update_placeholder();
    update_location_icon();

    m_omnibox.on_display_change = [this](WebView::Omnibox::Display const& display) {
        auto display_text = qstring_from_ak_string(display.text);
        if (text() != display_text)
            setText(display_text);

        if (display.selection_start.has_value()) {
            auto selection_start = qstring_offset_for_byte_offset(display.text, *display.selection_start);
            setSelection(selection_start, static_cast<int>(display_text.length()) - selection_start);
        } else {
            deselect();
            setCursorPosition(static_cast<int>(display_text.length()));
        }
    };

    m_omnibox.on_suggestions_change = [this] {
        if (!m_omnibox.is_popup_visible()) {
            m_autocomplete->close();
            return;
        }
        m_autocomplete->show_with_suggestions(m_omnibox.suggestions(), m_omnibox.selected_suggestion());
    };

    m_omnibox.on_selection_change = [this] {
        m_autocomplete->set_selected_suggestion(m_omnibox.selected_suggestion());
    };

    m_omnibox.on_commit = [this](String const& input) {
        auto input_text = qstring_from_ak_string(input);
        if (text() != input_text)
            setText(input_text);

        clearFocus();

        auto ctrl_held = QApplication::keyboardModifiers() & Qt::ControlModifier;
        auto append_tld = ctrl_held ? WebView::AppendTLD::Yes : WebView::AppendTLD::No;

        auto url = WebView::sanitize_url(input, WebView::Application::settings().search_engine(), append_tld);
        set_url(AK::move(url));

        emit returnPressed();
    };

    connect(m_autocomplete, &Autocomplete::suggestion_clicked, this, [this](int suggestion_index) {
        m_omnibox.suggestion_clicked(static_cast<size_t>(suggestion_index));
    });

    connect(m_autocomplete, &Autocomplete::suggestion_hovered, this, [this](int suggestion_index) {
        m_omnibox.suggestion_hovered(static_cast<size_t>(suggestion_index));
    });

    connect(m_autocomplete, &Autocomplete::dismissed, this, [this] {
        m_omnibox.popup_dismissed();
    });

    connect(this, &QLineEdit::textEdited, this, [this] {
        if (m_url_is_hidden)
            m_has_user_edited_hidden_url = true;

        bool cursor_at_end = !hasSelectedText() && cursorPosition() == text().length();
        m_omnibox.text_edited(ak_string_from_qstring(text()), cursor_at_end);
    });

    connect(this, &QLineEdit::textChanged, this, [this] {
        highlight_location();
        update_location_icon();
    });

    connect(this, &QLineEdit::cursorPositionChanged, this, [this](int, int new_position) {
        // A selection reaching the end of the text (an inline completion, select-all on focus) still
        // counts as "at the end": replacing it appends, and completions may be applied over it.
        bool at_end = new_position == text().length()
            && (!hasSelectedText() || selectionEnd() == text().length());
        m_omnibox.cursor_moved(at_end);
    });

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        schedule_chrome_style_update();
    });
#endif
}

void LocationEdit::set_trailing_action(QAction* action)
{
    m_trailing_action_button->setDefaultAction(action);
    m_trailing_action_button->setVisible(action != nullptr);
    update_text_margins();
    update_trailing_item_positions();
}

QAction* LocationEdit::trailing_action() const
{
    return m_trailing_action_button->defaultAction();
}

void LocationEdit::set_zoom_action(QAction* action)
{
    if (m_zoom_action == action)
        return;

    if (m_zoom_action)
        QObject::disconnect(m_zoom_action, nullptr, this, nullptr);

    m_zoom_action = action;

    if (m_zoom_action)
        connect(m_zoom_action, &QAction::changed, this, &LocationEdit::update_zoom_indicator);

    update_zoom_indicator();
}

void LocationEdit::set_url_is_hidden(bool url_is_hidden)
{
    if (m_url_is_hidden == url_is_hidden)
        return;

    m_url_is_hidden = url_is_hidden;
    m_has_user_edited_hidden_url = false;

    if (m_url_is_hidden)
        set_text_and_restart_editing({});
}

void LocationEdit::show_autocomplete()
{
    if (!window() || !window()->isVisible())
        return;

    m_omnibox.show_all_suggestions();
}

void LocationEdit::changeEvent(QEvent* event)
{
    QLineEdit::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::ThemeChange) {
        schedule_chrome_style_update();
    }
}

void LocationEdit::focusInEvent(QFocusEvent* event)
{
    if (event->reason() == Qt::PopupFocusReason) {
        QLineEdit::focusInEvent(event);
        m_omnibox.set_suspended(false);
        return;
    }

    auto should_defer_full_url = event->reason() == Qt::MouseFocusReason
        && m_url.has_value()
        && text() == display_url();

    QLineEdit::focusInEvent(event);

    m_should_show_full_url_on_mouse_release = should_defer_full_url;

    if (!should_defer_full_url && m_url.has_value() && text() == display_url())
        setText(serialized_url());

    m_omnibox.begin_editing(ak_string_from_qstring(text()));

    highlight_location();
    animate_focus_glow(58);

    if (!should_defer_full_url) {
        QTimer::singleShot(0, this, [this] {
            if (hasFocus())
                selectAll();
        });
    }
}

void LocationEdit::focusOutEvent(QFocusEvent* event)
{
    QLineEdit::focusOutEvent(event);

    // A popup (for example the context menu) borrows focus without ending the editing session; late
    // suggestion deliveries must not rewrite the text or raise the suggestion popup while it is open.
    if (event->reason() == Qt::PopupFocusReason) {
        m_omnibox.set_suspended(true);
        return;
    }

    animate_focus_glow(0);

    m_omnibox.end_editing();
    m_should_show_full_url_on_mouse_release = false;

    if (m_url_is_hidden) {
        m_url_is_hidden = false;
        m_has_user_edited_hidden_url = false;
        if (text().isEmpty() && m_url.has_value())
            setText(display_url());
    } else if (m_url.has_value() && text() == serialized_url()) {
        setText(display_url());
    }

    deselect();
    setCursorPosition(0);
    highlight_location();
}

void LocationEdit::update_focus_glow(int alpha)
{
    m_focus_glow_alpha = alpha;
    if (m_focus_glow_effect)
        m_focus_glow_effect->setColor(location_focus_glow_color(palette(), alpha));
}

void LocationEdit::animate_focus_glow(int target_alpha)
{
    if (!m_focus_glow_animation)
        return;

    m_focus_glow_animation->stop();
    m_focus_glow_animation->setStartValue(m_focus_glow_alpha);
    m_focus_glow_animation->setEndValue(target_alpha);
    m_focus_glow_animation->start();
}

void LocationEdit::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape) {
        if (m_omnibox.escape_pressed() == WebView::Omnibox::EscapeAction::ClosedPopup)
            return;
        if (m_url.has_value())
            setText(serialized_url());
        clearFocus();
        return;
    }

    if (event->key() == Qt::Key_Down) {
        if (m_omnibox.select_next_suggestion())
            return;
    }

    if (event->key() == Qt::Key_Up) {
        if (m_omnibox.select_previous_suggestion())
            return;
    }

    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        m_omnibox.return_pressed();
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_Backspace || event->key() == Qt::Key_Delete)
        m_omnibox.will_delete_text();

    QLineEdit::keyPressEvent(event);
}

void LocationEdit::mouseReleaseEvent(QMouseEvent* event)
{
    QLineEdit::mouseReleaseEvent(event);

    if (event->button() == Qt::LeftButton && m_should_show_full_url_on_mouse_release)
        show_full_url_preserving_display_selection();
}

void LocationEdit::resizeEvent(QResizeEvent* event)
{
    QLineEdit::resizeEvent(event);

    update_trailing_item_positions();
}

void LocationEdit::update_trailing_item_positions()
{
    auto button_size = m_leading_icon_button->size();
    auto y = (height() - button_size.height()) / 2 + (m_leading_icon_button->property("notSecure").toBool() ? 0 : 1);
    m_leading_icon_button->move(12, y);

    auto trailing_button_size = m_trailing_action_button->size();
    auto trailing_x = width() - trailing_button_size.width() - LOCATION_TRAILING_EDGE_MARGIN;
    auto trailing_y = (height() - trailing_button_size.height()) / 2;
    m_trailing_action_button->move(trailing_x, trailing_y);

    auto zoom_button_size = m_zoom_indicator_button->size();
    auto zoom_y = (height() - zoom_button_size.height()) / 2;
    m_zoom_indicator_button->move(trailing_x - LOCATION_TRAILING_ITEM_GAP - zoom_button_size.width(), zoom_y);
    m_zoom_indicator_button->raise();
    m_trailing_action_button->raise();
}

void LocationEdit::search_engine_changed()
{
    update_placeholder();
    update_location_icon();
}

void LocationEdit::update_text_margins()
{
    setTextMargins(m_text_leading_margin, 0, trailing_text_margin(), 0);
}

int LocationEdit::trailing_text_margin() const
{
    auto margin = LOCATION_TRAILING_EDGE_MARGIN + LOCATION_TRAILING_ACTION_WIDTH + LOCATION_TRAILING_TEXT_GAP;

    if (m_zoom_indicator_button && !m_zoom_indicator_button->isHidden())
        margin += m_zoom_indicator_button->width() + LOCATION_TRAILING_ITEM_GAP;

    return margin;
}

void LocationEdit::update_chrome_style()
{
    if (m_is_updating_chrome_style)
        return;

    m_is_updating_chrome_style = true;
    setStyleSheet(ChromeStyle::location_edit_style_sheet(palette()));
    m_is_updating_chrome_style = false;
    update_zoom_indicator();
}

void LocationEdit::schedule_chrome_style_update()
{
    if (m_has_pending_chrome_style_update)
        return;

    m_has_pending_chrome_style_update = true;
    QTimer::singleShot(0, this, [this] {
        update_chrome_style();
        m_autocomplete->schedule_chrome_style_update();
        update_focus_glow(m_focus_glow_alpha);
        update_location_icon();
        update_zoom_indicator();
        highlight_location();
        update();
        m_has_pending_chrome_style_update = false;
    });
}

void LocationEdit::update_placeholder()
{
    if (auto const& search_engine = WebView::Application::settings().search_engine(); search_engine.has_value()) {
        auto prompt = MUST(String::formatted("Search with {} or enter web address", search_engine->name));
        setPlaceholderText(qstring_from_ak_string(prompt));
    } else {
        setPlaceholderText("Enter web address");
    }
}

void LocationEdit::update_location_icon()
{
    if (!m_leading_icon_button)
        return;

    auto update_indicator_style = [this](bool not_secure) {
        if (m_leading_icon_button->property("notSecure").toBool() == not_secure)
            return;

        m_leading_icon_button->setProperty("notSecure", not_secure);
        m_leading_icon_button->style()->unpolish(m_leading_icon_button);
        m_leading_icon_button->style()->polish(m_leading_icon_button);
    };

    auto position_indicator = [this](int y_offset) {
        auto button_size = m_leading_icon_button->size();
        auto y = (height() - button_size.height()) / 2 + y_offset;
        m_leading_icon_button->move(12, y);
    };

    auto hide_indicator = [&] {
        update_indicator_style(false);
        m_leading_icon_button->hide();
        m_leading_icon_button->setText({});
        m_leading_icon_button->setIcon({});
        m_leading_icon_button->setToolTip({});
        m_text_leading_margin = 0;
        update_text_margins();
    };

    auto show_icon = [&](ChromeIcon icon, QString const& tooltip) {
        update_indicator_style(false);
        m_leading_icon_button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        m_leading_icon_button->setText({});
        m_leading_icon_button->setIcon(create_chrome_icon(icon, palette()));
        m_leading_icon_button->setIconSize({ 18, 18 });
        m_leading_icon_button->setFixedSize(22, 22);
        m_leading_icon_button->setToolTip(tooltip);
        m_leading_icon_button->show();
        position_indicator(1);
        m_text_leading_margin = 22;
        update_text_margins();
    };

    auto show_not_secure_indicator = [&] {
        update_indicator_style(true);
        m_leading_icon_button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        m_leading_icon_button->setIcon({});
        m_leading_icon_button->setText("Not secure");

        auto width = m_leading_icon_button->fontMetrics().horizontalAdvance("Not secure") + 18;
        m_leading_icon_button->setFixedSize(width, 22);
        m_leading_icon_button->setToolTip("Not secure");
        m_leading_icon_button->show();
        position_indicator(0);
        m_text_leading_margin = width;
        update_text_margins();
    };

    auto is_showing_current_url_for_display = !hasFocus()
        && m_url.has_value()
        && text() == display_url();

    if (text_matches_current_url() || is_showing_current_url_for_display) {
        auto const& scheme = m_url->scheme();
        if (scheme == "http"sv)
            show_not_secure_indicator();
        else
            hide_indicator();
        return;
    }

    auto query = ak_string_from_qstring(current_query());
    auto query_view = query.bytes_as_string_view().trim_whitespace();
    if (query_view.is_empty()) {
        hide_indicator();
    } else if (WebView::location_looks_like_url(query_view)) {
        show_icon(ChromeIcon::Globe, "Go to address");
    } else if (WebView::Application::settings().search_engine().has_value()) {
        show_icon(ChromeIcon::Search, "Search");
    } else {
        hide_indicator();
    }
}

void LocationEdit::update_zoom_indicator()
{
    if (!m_zoom_indicator_button)
        return;

    auto visible = m_zoom_action && m_zoom_action->isVisible() && !m_zoom_action->text().isEmpty();
    if (!visible) {
        m_zoom_indicator_button->hide();
        update_text_margins();
        update_trailing_item_positions();
        return;
    }

    m_zoom_indicator_button->setText(m_zoom_action->text());
    m_zoom_indicator_button->setToolTip(m_zoom_action->toolTip());

    auto width = m_zoom_indicator_button->fontMetrics().horizontalAdvance(m_zoom_indicator_button->text()) + LOCATION_PILL_HORIZONTAL_PADDING;
    m_zoom_indicator_button->setFixedSize(width, LOCATION_PILL_HEIGHT);
    m_zoom_indicator_button->show();

    update_text_margins();
    update_trailing_item_positions();
}

void LocationEdit::highlight_location()
{
    auto url = ak_string_from_qstring(text());
    QList<QInputMethodEvent::Attribute> attributes;

    auto darkened_text_color = ChromeStyle::chrome_text(palette());
    darkened_text_color.setAlpha(127);

    QTextCharFormat dark_attributes;
    dark_attributes.setForeground(darkened_text_color);

    QTextCharFormat highlight_attributes;
    highlight_attributes.setForeground(ChromeStyle::chrome_text(palette()));

    auto append_attributes = [&](StringView scheme_and_subdomain, StringView effective_tld_plus_one, StringView remainder) {
        attributes.append({
            QInputMethodEvent::TextFormat,
            -cursorPosition(),
            static_cast<int>(scheme_and_subdomain.length()),
            dark_attributes,
        });

        attributes.append({
            QInputMethodEvent::TextFormat,
            static_cast<int>(scheme_and_subdomain.length() - cursorPosition()),
            static_cast<int>(effective_tld_plus_one.length()),
            highlight_attributes,
        });

        attributes.append({
            QInputMethodEvent::TextFormat,
            static_cast<int>(scheme_and_subdomain.length() + effective_tld_plus_one.length() - cursorPosition()),
            static_cast<int>(remainder.length()),
            dark_attributes,
        });
    };

    if (m_url.has_value() && text() == display_url() && m_url->scheme().is_one_of("http"sv, "https"sv)) {
        auto serialized_url = m_url->serialize();
        if (auto url_parts = WebView::break_url_into_parts(serialized_url); url_parts.has_value()) {
            auto scheme_and_subdomain = url_parts->scheme_and_subdomain;
            auto remainder = url_parts->remainder;

            auto scheme_prefix_length = m_url->scheme().bytes_as_string_view().length() + "://"sv.length();
            if (scheme_and_subdomain.length() >= scheme_prefix_length)
                scheme_and_subdomain = scheme_and_subdomain.substring_view(scheme_prefix_length);
            if (scheme_and_subdomain.starts_with("www."sv, CaseSensitivity::CaseInsensitive))
                scheme_and_subdomain = scheme_and_subdomain.substring_view(4);
            if (remainder == "/"sv)
                remainder = {};

            append_attributes(scheme_and_subdomain, url_parts->effective_tld_plus_one, remainder);
        }
    } else if (auto url_parts = WebView::break_url_into_parts(url); url_parts.has_value()) {
        append_attributes(url_parts->scheme_and_subdomain, url_parts->effective_tld_plus_one, url_parts->remainder);
    }

    QInputMethodEvent event(QString(), attributes);
    QCoreApplication::sendEvent(this, &event);
}

// Programmatic text changes bypass textEdited, so the editing session must restart for the model to act
// on what the bar now shows.
void LocationEdit::set_text_and_restart_editing(QString const& text)
{
    setText(text);
    if (!m_omnibox.is_editing())
        return;

    // Without focus, the session is alive because a popup (for example the context menu) borrowed input;
    // the restarted session must stay suspended behind it.
    bool suspended = !hasFocus();
    m_omnibox.begin_editing(ak_string_from_qstring(text));
    m_omnibox.set_suspended(suspended);
}

void LocationEdit::set_url(Optional<URL::URL> url)
{
    m_url = AK::move(url);

    if (m_url_is_hidden) {
        if (!m_has_user_edited_hidden_url)
            set_text_and_restart_editing({});
    } else if (m_url.has_value()) {
        set_text_and_restart_editing(hasFocus() ? serialized_url() : display_url());
        setCursorPosition(0);
    }

    update_location_icon();
}

void LocationEdit::show_full_url_preserving_display_selection()
{
    if (!m_should_show_full_url_on_mouse_release)
        return;

    m_should_show_full_url_on_mouse_release = false;

    if (!m_url.has_value() || text() != display_url())
        return;

    auto selection_start = selectionStart();
    auto selection_length = selectedText().length();

    set_text_and_restart_editing(serialized_url());

    if (selection_start != -1) {
        auto serialized_selection_start = serialized_url_position_for_display_position(selection_start);
        auto serialized_selection_end = serialized_url_position_for_display_position(selection_start + selection_length);
        setSelection(serialized_selection_start, serialized_selection_end - serialized_selection_start);
    } else {
        selectAll();
    }

    highlight_location();
}

int LocationEdit::serialized_url_position_for_display_position(int display_position) const
{
    VERIFY(m_url.has_value());

    auto display = display_url();
    auto serialized = serialized_url();
    display_position = qBound(0, display_position, display.length());

    if (display == serialized || !m_url->scheme().is_one_of("http"sv, "https"sv))
        return min(display_position, serialized.length());

    int display_index = 0;
    int last_serialized_position = 0;
    auto map_visible_range = [&](int serialized_start, int length) -> Optional<int> {
        if (length <= 0)
            return {};

        if (display_position < display_index + length)
            return serialized_start + display_position - display_index;

        display_index += length;
        last_serialized_position = serialized_start + length;
        return {};
    };

    auto serialized_index = qstring_from_ak_string(m_url->scheme()).length() + "://"sv.length();

    if (!m_url->username().is_empty() || !m_url->password().is_empty()) {
        auto username = qstring_from_ak_string(m_url->username());
        auto password = qstring_from_ak_string(m_url->password());
        auto userinfo_length = username.length() + 1;
        if (!password.isEmpty())
            userinfo_length += 1 + password.length();

        if (auto position = map_visible_range(serialized_index, userinfo_length); position.has_value())
            return *position;
        serialized_index += userinfo_length;
    }

    auto host = qstring_from_ak_string(m_url->serialized_host());
    auto host_offset = host.startsWith("www.", Qt::CaseInsensitive) ? 4 : 0;
    if (auto position = map_visible_range(serialized_index + host_offset, host.length() - host_offset); position.has_value())
        return *position;
    serialized_index += host.length();

    if (auto port = m_url->port(); port.has_value()) {
        auto port_text = QString::number(*port);
        auto port_length = 1 + port_text.length();
        if (auto position = map_visible_range(serialized_index, port_length); position.has_value())
            return *position;
        serialized_index += port_length;
    }

    auto path = qstring_from_ak_string(m_url->serialize_path());
    if (path != "/" || m_url->query().has_value() || m_url->fragment().has_value()) {
        if (auto position = map_visible_range(serialized_index, path.length()); position.has_value())
            return *position;
    }
    serialized_index += path.length();

    if (m_url->query().has_value()) {
        auto query_length = 1 + qstring_from_ak_string(*m_url->query()).length();
        if (auto position = map_visible_range(serialized_index, query_length); position.has_value())
            return *position;
        serialized_index += query_length;
    }

    if (m_url->fragment().has_value()) {
        auto fragment_length = 1 + qstring_from_ak_string(*m_url->fragment()).length();
        if (auto position = map_visible_range(serialized_index, fragment_length); position.has_value())
            return *position;
    }

    return last_serialized_position;
}

bool LocationEdit::text_matches_current_url() const
{
    return m_url.has_value()
        && !m_url_is_hidden
        && (text() == serialized_url() || text() == display_url());
}

QString LocationEdit::serialized_url() const
{
    VERIFY(m_url.has_value());
    return qstring_from_ak_string(m_url->serialize());
}

QString LocationEdit::display_url() const
{
    VERIFY(m_url.has_value());
    return qstring_from_ak_string(WebView::url_for_display(*m_url));
}

QString LocationEdit::current_query() const
{
    if (m_omnibox.is_editing())
        return qstring_from_ak_string(m_omnibox.query());
    return text();
}

}
