/*
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/URL.h>
#include <LibWebView/Application.h>
#include <LibWebView/URL.h>
#include <UI/Qt/Autocomplete.h>
#include <UI/Qt/LocationEdit.h>
#include <UI/Qt/StringUtils.h>

#include <QApplication>
#include <QPalette>
#include <QTextLayout>
#include <QTimer>

namespace Ladybird {

LocationEdit::LocationEdit(QWidget* parent)
    : QLineEdit(parent)
    , m_autocomplete(new Autocomplete(this))
{
    update_placeholder();

    setCompleter(m_autocomplete);

    connect(m_autocomplete, QOverload<QModelIndex const&>::of(&QCompleter::activated), [&](QModelIndex const&) {
        emit returnPressed();
    });

    connect(this, &QLineEdit::returnPressed, [&] {
        if (text().isEmpty())
            return;

        clearFocus();

        Optional<StringView> search_engine_url;
        if (auto const& search_engine = WebView::Application::settings().search_engine(); search_engine.has_value())
            search_engine_url = search_engine->query_url;

        auto query = ak_string_from_qstring(text());

        auto ctrl_held = QApplication::keyboardModifiers() & Qt::ControlModifier;
        auto append_tld = ctrl_held ? WebView::AppendTLD::Yes : WebView::AppendTLD::No;

        if (auto url = WebView::sanitize_url(query, search_engine_url, append_tld); url.has_value())
            set_url(url.release_value());
    });

    connect(this, &QLineEdit::textEdited, [this] {
        m_autocomplete->query_autocomplete_engine(ak_string_from_qstring(text()));
    });

    connect(this, &QLineEdit::textChanged, this, &LocationEdit::highlight_location);
}

void LocationEdit::focusInEvent(QFocusEvent* event)
{
    QLineEdit::focusInEvent(event);
    highlight_location();

    if (event->reason() != Qt::PopupFocusReason)
        QTimer::singleShot(0, this, &QLineEdit::selectAll);
}

void LocationEdit::focusOutEvent(QFocusEvent* event)
{
    QLineEdit::focusOutEvent(event);

    if (m_url_is_hidden) {
        m_url_is_hidden = false;
        if (text().isEmpty())
            setText(qstring_from_ak_string(m_url.serialize()));
    }

    if (event->reason() != Qt::PopupFocusReason) {
        setCursorPosition(0);
        highlight_location();
    }
}

void LocationEdit::search_engine_changed()
{
    update_placeholder();
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

void LocationEdit::highlight_location()
{
    auto url = ak_string_from_qstring(text());
    QList<QInputMethodEvent::Attribute> attributes;

    if (auto url_parts = WebView::break_url_into_parts(url); url_parts.has_value()) {
        auto darkened_text_color = QPalette().color(QPalette::Text);
        darkened_text_color.setAlpha(127);

        QTextCharFormat dark_attributes;
        dark_attributes.setForeground(darkened_text_color);

        QTextCharFormat highlight_attributes;
        highlight_attributes.setForeground(QPalette().color(QPalette::Text));

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

void LocationEdit::set_url(URL::URL url)
{
    m_url = AK::move(url);

    if (m_url_is_hidden) {
        clear();
    } else {
        setText(qstring_from_ak_string(m_url.serialize()));
        setCursorPosition(0);
    }
}

}
