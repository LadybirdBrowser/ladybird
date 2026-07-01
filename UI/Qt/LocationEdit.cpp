/*
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/StringView.h>
#include <LibURL/URL.h>
#include <LibWebView/Application.h>
#include <LibWebView/Autocomplete.h>
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
#include <QLatin1String>
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

static QString candidate_by_trimming_root_trailing_slash(QString const& candidate)
{
    if (!candidate.endsWith(QLatin1Char('/')))
        return candidate;

    QString host_and_path = candidate;
    for (auto scheme : { QLatin1String("https://"), QLatin1String("http://") }) {
        if (host_and_path.startsWith(scheme)) {
            host_and_path = host_and_path.mid(scheme.size());
            break;
        }
    }

    int first_slash = host_and_path.indexOf(QLatin1Char('/'));
    if (first_slash == -1 || first_slash != host_and_path.length() - 1)
        return candidate;

    return candidate.left(candidate.length() - 1);
}

static bool query_matches_candidate_exactly(QString const& query, QString const& candidate)
{
    auto trimmed = candidate_by_trimming_root_trailing_slash(candidate);
    return trimmed.compare(query, Qt::CaseInsensitive) == 0;
}

static QString inline_autocomplete_text_for_candidate(QString const& query, QString const& candidate)
{
    if (query.isEmpty() || candidate.length() <= query.length())
        return {};
    if (!candidate.startsWith(query, Qt::CaseInsensitive))
        return {};
    return query + candidate.mid(query.length());
}

static QString inline_autocomplete_text_for_suggestion(QString const& query, QString const& suggestion_text)
{
    auto trimmed = candidate_by_trimming_root_trailing_slash(suggestion_text);

    if (auto direct = inline_autocomplete_text_for_candidate(query, trimmed); !direct.isEmpty())
        return direct;

    if (trimmed.startsWith(QLatin1String("www."))) {
        auto stripped = trimmed.mid(4);
        if (auto match = inline_autocomplete_text_for_candidate(query, stripped); !match.isEmpty())
            return match;
    }

    for (auto scheme : { QLatin1String("https://"), QLatin1String("http://") }) {
        if (!trimmed.startsWith(scheme))
            continue;
        auto stripped = trimmed.mid(scheme.size());
        if (auto match = inline_autocomplete_text_for_candidate(query, stripped); !match.isEmpty())
            return match;
        if (stripped.startsWith(QLatin1String("www."))) {
            auto stripped_www = stripped.mid(4);
            if (auto match = inline_autocomplete_text_for_candidate(query, stripped_www); !match.isEmpty())
                return match;
        }
    }

    return {};
}

static bool suggestion_matches_query_exactly(QString const& query, QString const& suggestion_text)
{
    auto trimmed = candidate_by_trimming_root_trailing_slash(suggestion_text);
    if (query_matches_candidate_exactly(query, trimmed))
        return true;

    if (trimmed.startsWith(QLatin1String("www."))) {
        if (query_matches_candidate_exactly(query, trimmed.mid(4)))
            return true;
    }

    for (auto scheme : { QLatin1String("https://"), QLatin1String("http://") }) {
        if (!trimmed.startsWith(scheme))
            continue;
        auto stripped = trimmed.mid(scheme.size());
        if (query_matches_candidate_exactly(query, stripped))
            return true;
        if (stripped.startsWith(QLatin1String("www."))
            && query_matches_candidate_exactly(query, stripped.mid(4)))
            return true;
    }

    return false;
}

static int autocomplete_suggestion_index(QString const& suggestion_text, Vector<WebView::AutocompleteSuggestion> const& suggestions)
{
    for (size_t i = 0; i < suggestions.size(); ++i) {
        if (qstring_from_ak_string(suggestions[i].text) == suggestion_text)
            return static_cast<int>(i);
    }
    return -1;
}

static bool should_suppress_inline_autocomplete_for_key(QKeyEvent const* event)
{
    auto key = event->key();
    return key == Qt::Key_Backspace || key == Qt::Key_Delete;
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

    m_autocomplete->on_query_complete = [this](auto suggestions, WebView::AutocompleteResultKind result_kind) {
        if (!hasFocus())
            return;

        auto query = autocomplete_query();
        int selected_row = -1;
        if (!m_autocomplete_preview_query.isNull()) {
            for (size_t i = 0; i < suggestions.size(); ++i) {
                if (qstring_from_ak_string(suggestions[i].text) == text()) {
                    selected_row = static_cast<int>(i);
                    break;
                }
            }
        } else if (!m_autocomplete_query_without_inline.isNull() && text() == m_autocomplete_query_without_inline) {
            selected_row = 0;
        } else {
            selected_row = apply_inline_autocomplete(suggestions);
        }

        // Do not update the popup while results are still changing.
        // Intermediate updates are triggered on every keystroke and would
        // cause visible flicker in the suggestion list.
        // Only final results are used to refresh the UI.
        bool should_activate_pending_query = !m_pending_autocomplete_activation_query.isNull()
            && m_pending_autocomplete_activation_query == query;
        if (result_kind == WebView::AutocompleteResultKind::Intermediate && m_autocomplete->is_visible() && !should_activate_pending_query)
            return;

        m_autocomplete_popup_query = query;
        m_autocomplete->show_with_suggestions(AK::move(suggestions), selected_row);
        if (should_activate_pending_query) {
            auto selected = m_autocomplete->selected_suggestion();
            if (result_kind == WebView::AutocompleteResultKind::Final
                || (selected.has_value() && qstring_from_ak_string(*selected) != query)) {
                m_pending_autocomplete_activation_query = QString();
                m_should_skip_autocomplete_cancel_on_focus_out = true;
                activate_selected_autocomplete_suggestion();
            }
        }
    };

    connect(m_autocomplete, &Autocomplete::suggestion_activated, this, [this](QString const& text) {
        m_autocomplete_preview_query = QString();
        m_current_inline_autocomplete_suggestion.clear();
        set_text_without_inline_autocomplete(text);
        m_autocomplete->close();
        emit returnPressed();
    });

    connect(m_autocomplete, &Autocomplete::suggestion_highlighted, this, [this](QString const& text) {
        m_has_highlighted_autocomplete_suggestion = true;
        auto query = autocomplete_query();
        apply_inline_autocomplete_suggestion_text(text, query, true);
    });

    connect(m_autocomplete, &Autocomplete::did_close, this, [this] {
        auto should_preserve_inline_autocomplete = m_should_preserve_inline_autocomplete_on_close;
        auto should_restore_query = should_restore_autocomplete_query();
        m_should_preserve_inline_autocomplete_on_close = false;
        m_has_highlighted_autocomplete_suggestion = false;
        m_should_submit_current_text_on_return = false;
        m_current_inline_autocomplete_suggestion.clear();
        if (should_preserve_inline_autocomplete)
            return;
        if (!m_autocomplete_query_without_inline.isNull()) {
            if (should_restore_query)
                restore_query();
            m_autocomplete_query_without_inline = QString();
            return;
        }
        restore_query();
    });

    connect(this, &QLineEdit::returnPressed, this, [this] {
        if (text().isEmpty())
            return;

        auto query = ak_string_from_qstring(text());

        reset_autocomplete_state();
        clearFocus();

        auto ctrl_held = QApplication::keyboardModifiers() & Qt::ControlModifier;
        auto append_tld = ctrl_held ? WebView::AppendTLD::Yes : WebView::AppendTLD::No;

        auto url = WebView::sanitize_url(query, WebView::Application::settings().search_engine(), append_tld);
        set_url(AK::move(url));
    });

    connect(this, &QLineEdit::textEdited, this, [this] {
        if (m_is_applying_inline_autocomplete)
            return;

        bool had_autocomplete_preview = !m_autocomplete_preview_query.isNull();
        bool had_inline_autocomplete = !m_current_inline_autocomplete_suggestion.isEmpty();

        m_autocomplete_query_without_inline = QString();
        m_autocomplete_preview_query = QString();
        m_has_highlighted_autocomplete_suggestion = false;
        if (had_autocomplete_preview)
            m_should_submit_current_text_on_return = true;

        if (m_url_is_hidden)
            m_has_user_edited_hidden_url = true;

        auto query = current_query();
        if (!m_pending_autocomplete_activation_query.isNull() && m_pending_autocomplete_activation_query != query)
            m_pending_autocomplete_activation_query = QString();

        if (m_should_suppress_inline_autocomplete_on_next_change) {
            if (had_inline_autocomplete)
                m_should_submit_current_text_on_return = true;
            m_suppressed_inline_autocomplete_query = query;
            m_should_suppress_inline_autocomplete_on_next_change = false;
        } else if (!m_suppressed_inline_autocomplete_query.isNull()
            && m_suppressed_inline_autocomplete_query != query) {
            m_suppressed_inline_autocomplete_query = QString();
        }

        if (m_suppressed_inline_autocomplete_query.isNull()
            && !m_current_inline_autocomplete_suggestion.isEmpty()) {
            if (!apply_inline_autocomplete_suggestion_text(m_current_inline_autocomplete_suggestion, query)) {
                m_current_inline_autocomplete_suggestion.clear();
                if (had_inline_autocomplete)
                    m_should_submit_current_text_on_return = true;
            }
        }

        m_autocomplete->query_autocomplete_engine(ak_string_from_qstring(query));
    });

    connect(this, &QLineEdit::textChanged, this, [this] {
        highlight_location();
        update_location_icon();
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
        clear();
}

void LocationEdit::show_autocomplete()
{
    if (!window() || !window()->isVisible())
        return;

    auto query = text();
    m_autocomplete_popup_query = QString();
    m_autocomplete_query_without_inline = query;
    m_autocomplete->query_autocomplete_engine(ak_string_from_qstring(query));
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
        return;
    }

    auto should_defer_full_url = event->reason() == Qt::MouseFocusReason
        && m_url.has_value()
        && text() == display_url();

    QLineEdit::focusInEvent(event);

    m_should_show_full_url_on_mouse_release = should_defer_full_url;

    if (!should_defer_full_url && m_url.has_value() && text() == display_url())
        setText(serialized_url());

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

    if (event->reason() == Qt::PopupFocusReason)
        return;

    animate_focus_glow(0);

    if (should_restore_autocomplete_query()) {
        auto query = current_query();
        set_text_without_inline_autocomplete(query);
        setCursorPosition(query.length());
    }

    auto should_cancel_pending_query = !m_should_skip_autocomplete_cancel_on_focus_out;
    reset_autocomplete_state();
    if (should_cancel_pending_query)
        m_autocomplete->cancel_pending_query();
    m_autocomplete->close();
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
    if (event->reason() != Qt::PopupFocusReason) {
        setCursorPosition(0);
        highlight_location();
    }
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
        if (m_autocomplete->is_visible()
            && m_autocomplete_query_without_inline.isNull()
            && m_autocomplete_preview_query.isNull()
            && !m_has_highlighted_autocomplete_suggestion
            && hasSelectedText()) {
            m_should_preserve_inline_autocomplete_on_close = true;
        }
        if (m_autocomplete->close()) {
            m_autocomplete->cancel_pending_query();
            return;
        }
        reset_autocomplete_state();
        if (m_url.has_value())
            setText(serialized_url());
        clearFocus();
        return;
    }

    if (event->key() == Qt::Key_Down) {
        if (m_autocomplete->select_next_suggestion())
            return;
    }

    if (event->key() == Qt::Key_Up) {
        if (m_autocomplete->select_previous_suggestion())
            return;
    }

    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && m_autocomplete->is_visible()) {
        // Manual edits to automatic completion text should submit the edited text, not the popup's automatically selected row.
        if (m_should_submit_current_text_on_return && !m_has_highlighted_autocomplete_suggestion) {
            auto query = current_query();
            reset_autocomplete_state();
            set_text_without_inline_autocomplete(query);
            setCursorPosition(query.length());
            m_autocomplete->close();
            emit returnPressed();
            event->accept();
            return;
        }

        auto query = autocomplete_query();
        if (m_autocomplete_popup_query == query) {
            activate_selected_autocomplete_suggestion();
            event->accept();
            return;
        } else {
            m_pending_autocomplete_activation_query = query;
            m_autocomplete->query_autocomplete_engine(ak_string_from_qstring(query));
            event->accept();
            return;
        }
    }

    if (should_suppress_inline_autocomplete_for_key(event))
        m_should_suppress_inline_autocomplete_on_next_change = true;

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

void LocationEdit::set_url(Optional<URL::URL> url)
{
    m_url = AK::move(url);

    if (m_url_is_hidden) {
        if (!m_has_user_edited_hidden_url)
            clear();
    } else if (m_url.has_value()) {
        setText(hasFocus() ? serialized_url() : display_url());
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

    setText(serialized_url());

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
    if (!m_autocomplete_preview_query.isNull())
        return m_autocomplete_preview_query;

    if (!hasSelectedText())
        return text();
    int start = selectionStart();
    int length = selectedText().length();
    if (start + length != text().length())
        return text();
    return text().left(start);
}

int LocationEdit::apply_inline_autocomplete(Vector<WebView::AutocompleteSuggestion> const& suggestions)
{
    if (m_is_applying_inline_autocomplete) {
        dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] apply_inline_autocomplete: skipped (re-entrant)");
        return -1;
    }
    if (!hasFocus()) {
        dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] apply_inline_autocomplete: skipped (no focus)");
        return -1;
    }

    QString query;
    auto current_text = text();
    if (!hasSelectedText()) {
        if (cursorPosition() != current_text.length()) {
            dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] apply_inline_autocomplete: skipped (caret not at end)");
            return -1;
        }
        query = current_text;
    } else {
        int start = selectionStart();
        int end = start + selectedText().length();
        if (end != current_text.length()) {
            dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] apply_inline_autocomplete: skipped (selection not at end)");
            return -1;
        }
        query = current_text.left(start);
    }

    dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] apply_inline_autocomplete: query='{}' suggestions={}",
        ak_string_from_qstring(query),
        suggestions.size());
    for (size_t i = 0; i < suggestions.size(); ++i) {
        auto const& suggestion = suggestions[i];
        dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History]   [{}] source={} text='{}'",
            i,
            suggestion.source == WebView::AutocompleteSuggestionSource::LiteralURL    ? "LiteralURL"sv
                : suggestion.source == WebView::AutocompleteSuggestionSource::History ? "History"sv
                                                                                      : "Search"sv,
            suggestion.text);
    }

    if (suggestions.is_empty()) {
        dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] apply_inline_autocomplete: no suggestions, selected=-1");
        return -1;
    }

    // Row 0 drives both the visible highlight and (if its text prefix-matches
    // the query) the inline completion preview. This is a deliberate
    // simplification over the exact/inline/fallback fan-out we used to have:
    // the user-visible rule is "the top row is the default action".

    auto row_0_text_q = qstring_from_ak_string(suggestions.first().text);

    // A literal URL always wins: no preview, restore the typed text.
    if (suggestions.first().source == WebView::AutocompleteSuggestionSource::LiteralURL) {
        m_current_inline_autocomplete_suggestion.clear();
        if (hasSelectedText() || current_text != query)
            restore_query();
        dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] apply_inline_autocomplete: literal URL, selected=0");
        return 0;
    }

    // Backspace suppression: the user just deleted into this query, so don't
    // re-apply an inline preview — but still honor the "highlight the top row"
    // rule.
    if (!m_suppressed_inline_autocomplete_query.isNull() && m_suppressed_inline_autocomplete_query == query) {
        m_current_inline_autocomplete_suggestion.clear();
        if (hasSelectedText() || current_text != query)
            restore_query();
        dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] apply_inline_autocomplete: suppressed query, selected=0 (no preview)");
        return 0;
    }

    // Preserve an existing inline preview if its row is still present and
    // still extends the typed prefix. This keeps the preview stable while the
    // user is still forward-typing into a suggestion.
    if (!m_current_inline_autocomplete_suggestion.isEmpty()) {
        int preserved = autocomplete_suggestion_index(m_current_inline_autocomplete_suggestion, suggestions);
        if (preserved != -1) {
            auto preserved_inline = inline_autocomplete_text_for_suggestion(query, m_current_inline_autocomplete_suggestion);
            if (!preserved_inline.isEmpty()) {
                apply_inline_autocomplete_text(preserved_inline, query);
                dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] apply_inline_autocomplete: preserved inline row={} text='{}'",
                    preserved, ak_string_from_qstring(m_current_inline_autocomplete_suggestion));
                return preserved;
            }
        }
    }

    // Try to inline-preview row 0 specifically.
    auto row_0_inline = inline_autocomplete_text_for_suggestion(query, row_0_text_q);
    if (!row_0_inline.isEmpty()) {
        m_current_inline_autocomplete_suggestion = row_0_text_q;
        apply_inline_autocomplete_text(row_0_inline, query);
        dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] apply_inline_autocomplete: row 0 inline match, inline='{}'",
            ak_string_from_qstring(row_0_inline));
        return 0;
    }

    // Row 0 does not prefix-match the query: clear any stale inline preview,
    // restore the typed text, and still highlight row 0.
    m_current_inline_autocomplete_suggestion.clear();
    if (hasSelectedText() || current_text != query)
        restore_query();
    dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] apply_inline_autocomplete: row 0 not a prefix match, selected=0 (highlight only)");
    return 0;
}

bool LocationEdit::apply_inline_autocomplete_suggestion_text(QString const& suggestion_text, QString const& query, bool allow_preview)
{
    if (suggestion_matches_query_exactly(query, suggestion_text)) {
        restore_query();
        m_current_inline_autocomplete_suggestion.clear();
        return true;
    }

    auto inline_text = inline_autocomplete_text_for_suggestion(query, suggestion_text);
    if (inline_text.isEmpty() && allow_preview) {
        apply_autocomplete_preview_text(suggestion_text, query);
        return true;
    }
    if (inline_text.isEmpty())
        return false;

    m_current_inline_autocomplete_suggestion = suggestion_text;
    apply_inline_autocomplete_text(inline_text, query);
    return true;
}

void LocationEdit::apply_inline_autocomplete_text(QString const& inline_text, QString const& query)
{
    if (!hasFocus())
        return;

    int completion_start = query.length();
    int completion_length = inline_text.length() - query.length();
    if (completion_length <= 0)
        return;

    if (text() == inline_text && hasSelectedText()
        && selectionStart() == completion_start
        && selectedText().length() == completion_length)
        return;

    m_autocomplete_preview_query = QString();
    set_text_without_inline_autocomplete(inline_text);
    setSelection(completion_start, completion_length);
}

void LocationEdit::apply_autocomplete_preview_text(QString const& suggestion_text, QString const& query)
{
    if (!hasFocus())
        return;

    if (text() == suggestion_text && hasSelectedText()
        && selectionStart() == 0
        && selectedText().length() == suggestion_text.length())
        return;

    m_current_inline_autocomplete_suggestion.clear();
    m_autocomplete_preview_query = query;
    set_text_without_inline_autocomplete(suggestion_text);
    selectAll();
}

void LocationEdit::activate_selected_autocomplete_suggestion()
{
    if (auto selected = m_autocomplete->selected_suggestion(); selected.has_value()) {
        m_autocomplete_preview_query = QString();
        m_current_inline_autocomplete_suggestion.clear();
        set_text_without_inline_autocomplete(qstring_from_ak_string(*selected));
    }
    m_autocomplete->close();
    emit returnPressed();
}

void LocationEdit::restore_query()
{
    if (!hasFocus())
        return;

    auto query = current_query();
    if (text() == query && !hasSelectedText())
        return;

    set_text_without_inline_autocomplete(query);
    setCursorPosition(query.length());
    m_autocomplete_preview_query = QString();
}

void LocationEdit::set_text_without_inline_autocomplete(QString const& text)
{
    m_is_applying_inline_autocomplete = true;
    setText(text);
    m_is_applying_inline_autocomplete = false;
}

bool LocationEdit::should_restore_autocomplete_query() const
{
    return !m_autocomplete_preview_query.isNull() || !m_current_inline_autocomplete_suggestion.isEmpty();
}

QString LocationEdit::autocomplete_query() const
{
    if (!m_autocomplete_query_without_inline.isNull())
        return m_autocomplete_query_without_inline;
    return current_query();
}

void LocationEdit::reset_autocomplete_state()
{
    m_autocomplete_popup_query = QString();
    m_autocomplete_query_without_inline = QString();
    m_autocomplete_preview_query = QString();
    m_current_inline_autocomplete_suggestion.clear();
    m_has_highlighted_autocomplete_suggestion = false;
    m_should_submit_current_text_on_return = false;
    m_should_preserve_inline_autocomplete_on_close = false;
    m_should_skip_autocomplete_cancel_on_focus_out = false;
    m_pending_autocomplete_activation_query = QString();
    m_suppressed_inline_autocomplete_query = QString();
    m_should_suppress_inline_autocomplete_on_next_change = false;
}

}
