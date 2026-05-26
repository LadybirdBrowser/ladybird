/*
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
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
#include <QPalette>
#include <QResizeEvent>
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
#    include <QStyleHints>
#endif
#include <QTextLayout>
#include <QTimer>
#include <QToolButton>
#include <QVariantAnimation>

namespace Ladybird {

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

LocationEdit::LocationEdit(QWidget* parent)
    : QLineEdit(parent)
    , m_autocomplete(new Autocomplete(this))
{
    setObjectName("LadybirdLocationEdit");
    setMinimumHeight(37);
    setTextMargins(38, 0, 40, 0);
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
    m_leading_icon_button->setIconSize({ 20, 20 });
    m_leading_icon_button->setFixedSize(22, 22);
    m_leading_icon_button->setAutoRaise(true);
    m_leading_icon_button->setFocusPolicy(Qt::NoFocus);
    m_leading_icon_button->setCursor(Qt::ArrowCursor);
    m_leading_icon_button->setToolTip("Search");

    m_trailing_action_button = new QToolButton(this);
    m_trailing_action_button->setObjectName("LadybirdLocationAction");
    m_trailing_action_button->setIconSize({ 20, 20 });
    m_trailing_action_button->setFixedSize(24, 24);
    m_trailing_action_button->setAutoRaise(true);
    m_trailing_action_button->setFocusPolicy(Qt::NoFocus);
    m_trailing_action_button->setCursor(Qt::ArrowCursor);
    m_trailing_action_button->hide();

    m_loading_animation_timer = new QTimer(this);
    m_loading_animation_timer->setInterval(80);
    connect(m_loading_animation_timer, &QTimer::timeout, this, [this] {
        m_loading_animation_frame = (m_loading_animation_frame + 1) % 12;
        update_loading_icon();
    });

    update_placeholder();
    update_location_icon();

    m_autocomplete->on_query_complete = [this](auto suggestions, WebView::AutocompleteResultKind result_kind) {
        int selected_row = apply_inline_autocomplete(suggestions);

        if (result_kind == WebView::AutocompleteResultKind::Intermediate && m_autocomplete->is_visible()) {
            if (auto selected = m_autocomplete->selected_suggestion(); selected.has_value()) {
                for (auto const& suggestion : suggestions) {
                    if (suggestion.text == *selected)
                        return;
                }
            }
            m_autocomplete->clear_selection();
            return;
        }

        m_autocomplete->show_with_suggestions(AK::move(suggestions), selected_row);
    };

    connect(m_autocomplete, &Autocomplete::suggestion_activated, this, [this](QString const& text) {
        m_is_applying_inline_autocomplete = true;
        setText(text);
        m_is_applying_inline_autocomplete = false;
        m_autocomplete->close();
        emit returnPressed();
    });

    connect(m_autocomplete, &Autocomplete::suggestion_highlighted, this, [this](QString const& text) {
        auto query = current_query();
        apply_inline_autocomplete_suggestion_text(text, query);
    });

    connect(m_autocomplete, &Autocomplete::did_close, this, [this] {
        m_current_inline_autocomplete_suggestion.clear();
        restore_query();
    });

    connect(this, &QLineEdit::returnPressed, this, [this] {
        if (text().isEmpty())
            return;

        reset_autocomplete_state();
        clearFocus();

        auto query = ak_string_from_qstring(text());

        auto ctrl_held = QApplication::keyboardModifiers() & Qt::ControlModifier;
        auto append_tld = ctrl_held ? WebView::AppendTLD::Yes : WebView::AppendTLD::No;

        auto url = WebView::sanitize_url(query, WebView::Application::settings().search_engine(), append_tld);
        set_url(AK::move(url));
    });

    connect(this, &QLineEdit::textEdited, this, [this] {
        if (m_is_applying_inline_autocomplete)
            return;

        if (m_url_is_hidden)
            m_has_user_edited_hidden_url = true;

        auto query = current_query();

        if (m_should_suppress_inline_autocomplete_on_next_change) {
            m_suppressed_inline_autocomplete_query = query;
            m_should_suppress_inline_autocomplete_on_next_change = false;
        } else if (!m_suppressed_inline_autocomplete_query.isNull()
            && m_suppressed_inline_autocomplete_query != query) {
            m_suppressed_inline_autocomplete_query = QString();
        }

        if (m_suppressed_inline_autocomplete_query.isNull()
            && !m_current_inline_autocomplete_suggestion.isEmpty()) {
            if (!apply_inline_autocomplete_suggestion_text(m_current_inline_autocomplete_suggestion, query))
                m_current_inline_autocomplete_suggestion.clear();
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
}

QAction* LocationEdit::trailing_action() const
{
    return m_trailing_action_button->defaultAction();
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

void LocationEdit::changeEvent(QEvent* event)
{
    QLineEdit::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::ThemeChange) {
        schedule_chrome_style_update();
    }
}

void LocationEdit::focusInEvent(QFocusEvent* event)
{
    QLineEdit::focusInEvent(event);
    highlight_location();
    animate_focus_glow(58);

    if (event->reason() != Qt::PopupFocusReason)
        QTimer::singleShot(0, this, &QLineEdit::selectAll);
}

void LocationEdit::focusOutEvent(QFocusEvent* event)
{
    QLineEdit::focusOutEvent(event);
    animate_focus_glow(0);

    reset_autocomplete_state();
    m_autocomplete->cancel_pending_query();
    m_autocomplete->close();

    if (m_url_is_hidden) {
        m_url_is_hidden = false;
        m_has_user_edited_hidden_url = false;
        if (text().isEmpty() && m_url.has_value())
            setText(qstring_from_ak_string(m_url->serialize()));
    }

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
        if (m_autocomplete->close())
            return;
        reset_autocomplete_state();
        if (m_url.has_value())
            setText(qstring_from_ak_string(m_url->serialize()));
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
        if (auto selected = m_autocomplete->selected_suggestion(); selected.has_value()) {
            m_is_applying_inline_autocomplete = true;
            setText(qstring_from_ak_string(*selected));
            m_is_applying_inline_autocomplete = false;
        }
        m_autocomplete->close();
    }

    if (should_suppress_inline_autocomplete_for_key(event))
        m_should_suppress_inline_autocomplete_on_next_change = true;

    QLineEdit::keyPressEvent(event);
}

void LocationEdit::resizeEvent(QResizeEvent* event)
{
    QLineEdit::resizeEvent(event);

    auto button_size = m_leading_icon_button->sizeHint();
    auto y = (height() - button_size.height()) / 2 + 1;
    m_leading_icon_button->move(12, y);

    auto trailing_button_size = m_trailing_action_button->sizeHint();
    auto trailing_y = (height() - trailing_button_size.height()) / 2 + 1;
    m_trailing_action_button->move(width() - trailing_button_size.width() - 12, trailing_y);
}

void LocationEdit::search_engine_changed()
{
    update_placeholder();
}

void LocationEdit::update_chrome_style()
{
    if (m_is_updating_chrome_style)
        return;

    m_is_updating_chrome_style = true;
    setStyleSheet(ChromeStyle::location_edit_style_sheet(palette()));
    m_is_updating_chrome_style = false;
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

    if (m_is_loading) {
        update_loading_icon();
        return;
    }

    if (text_matches_current_url()) {
        m_leading_icon_button->setIcon(m_favicon.isNull() ? create_chrome_icon(ChromeIcon::Globe, palette()) : m_favicon);
        m_leading_icon_button->setToolTip("Page icon");
        return;
    }

    m_leading_icon_button->setIcon(create_chrome_icon(ChromeIcon::Search, palette()));
    m_leading_icon_button->setToolTip("Search");
}

void LocationEdit::update_loading_icon()
{
    if (!m_leading_icon_button)
        return;

    m_leading_icon_button->setIcon(loading_spinner_icon(palette(), m_loading_animation_frame));
    m_leading_icon_button->setToolTip("Loading");
}

void LocationEdit::highlight_location()
{
    auto url = ak_string_from_qstring(text());
    QList<QInputMethodEvent::Attribute> attributes;

    if (auto url_parts = WebView::break_url_into_parts(url); url_parts.has_value()) {
        auto darkened_text_color = ChromeStyle::chrome_text(palette());
        darkened_text_color.setAlpha(127);

        QTextCharFormat dark_attributes;
        dark_attributes.setForeground(darkened_text_color);

        QTextCharFormat highlight_attributes;
        highlight_attributes.setForeground(ChromeStyle::chrome_text(palette()));

        attributes.append({
            QInputMethodEvent::TextFormat,
            -cursorPosition(),
            static_cast<int>(url_parts->scheme_and_subdomain.length()),
            dark_attributes,
        });

        attributes.append({
            QInputMethodEvent::TextFormat,
            static_cast<int>(url_parts->scheme_and_subdomain.length() - cursorPosition()),
            static_cast<int>(url_parts->effective_tld_plus_one.length()),
            highlight_attributes,
        });

        attributes.append({
            QInputMethodEvent::TextFormat,
            static_cast<int>(url_parts->scheme_and_subdomain.length() + url_parts->effective_tld_plus_one.length() - cursorPosition()),
            static_cast<int>(url_parts->remainder.length()),
            dark_attributes,
        });
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
        setText(qstring_from_ak_string(m_url->serialize()));
        setCursorPosition(0);
    }

    update_location_icon();
}

void LocationEdit::set_loading(bool is_loading)
{
    if (m_is_loading == is_loading)
        return;

    m_is_loading = is_loading;
    m_loading_animation_frame = 0;

    if (m_is_loading)
        m_loading_animation_timer->start();
    else
        m_loading_animation_timer->stop();

    update_location_icon();
}

void LocationEdit::set_favicon(QIcon const& favicon)
{
    m_favicon = favicon;
    update_location_icon();
}

bool LocationEdit::text_matches_current_url() const
{
    return m_url.has_value()
        && !m_url_is_hidden
        && text() == qstring_from_ak_string(m_url->serialize());
}

QString LocationEdit::current_query() const
{
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

bool LocationEdit::apply_inline_autocomplete_suggestion_text(QString const& suggestion_text, QString const& query)
{
    if (suggestion_matches_query_exactly(query, suggestion_text)) {
        restore_query();
        m_current_inline_autocomplete_suggestion.clear();
        return true;
    }

    auto inline_text = inline_autocomplete_text_for_suggestion(query, suggestion_text);
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

    m_is_applying_inline_autocomplete = true;
    setText(inline_text);
    setSelection(completion_start, completion_length);
    m_is_applying_inline_autocomplete = false;
}

void LocationEdit::restore_query()
{
    if (!hasFocus())
        return;

    auto query = current_query();
    if (text() == query && !hasSelectedText())
        return;

    m_is_applying_inline_autocomplete = true;
    setText(query);
    setCursorPosition(query.length());
    m_is_applying_inline_autocomplete = false;
}

void LocationEdit::reset_autocomplete_state()
{
    m_current_inline_autocomplete_suggestion.clear();
    m_suppressed_inline_autocomplete_query = QString();
    m_should_suppress_inline_autocomplete_on_next_change = false;
}

}
