/*
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/URL.h>
#include <LibWebView/URL.h>
#include <UI/Qt/LocationEdit.h>
#include <UI/Qt/Settings.h>
#include <UI/Qt/StringUtils.h>

#include <QApplication>
#include <QPalette>
#include <QTextLayout>
#include <QTimer>

namespace Ladybird {

LocationEdit::LocationEdit(QWidget* parent)
    : QLineEdit(parent)
{
    update_placeholder();
    QObject::connect(Settings::the(), &Settings::enable_search_changed, this, &LocationEdit::update_placeholder);
    QObject::connect(Settings::the(), &Settings::search_engine_changed, this, &LocationEdit::update_placeholder);

    m_autocomplete = make<AutoComplete>(this);
    this->setCompleter(m_autocomplete);

    connect(m_autocomplete, &AutoComplete::activated, [&](QModelIndex const&) {
        emit returnPressed();
    });

    connect(this, &QLineEdit::returnPressed, [&] {
        if (text().isEmpty())
            return;

        clearFocus();

        auto query = ak_string_from_qstring(text());

        Optional<StringView> search_engine_url;
        if (Settings::the()->enable_search()) {
            search_engine_url = Settings::the()->search_engine().query_url;
            if (query.starts_with('!')) {
                auto splits = MUST(query.split(' ')); // FIXME: some missing error handling here.
                auto exist = WebView::find_search_engine_by_bang(splits[0]);
                if (exist.has_value()) {
                    search_engine_url = exist->query_url;
                    query = MUST(query.substring_from_byte_offset(splits[0].bytes().size()));
                }
            }
            // FIXME: low quality code. this can be merged into the if branch
            auto const temp = query.split(' ');
            if (!temp.is_error()) {
                auto const last = temp.value().last();
                if (temp.value().last().starts_with('!')) {
                    auto exist = WebView::find_search_engine_by_bang(last);
                    if (exist.has_value()) {
                        search_engine_url = exist->query_url;
                        query = MUST(query.substring_from_byte_offset(0, query.bytes().size() - last.bytes().size()));
                    }
                }
            }
        }

        if (auto url = WebView::sanitize_url(query, search_engine_url); url.has_value())
            set_url(url.release_value());
    });

    connect(this, &QLineEdit::textEdited, [this] {
        if (!Settings::the()->enable_autocomplete()) {
            m_autocomplete->clear_suggestions();
            return;
        }

        auto cursor_position = cursorPosition();

        m_autocomplete->get_search_suggestions(ak_string_from_qstring(text()));
        setCursorPosition(cursor_position);
    });

    connect(this, &QLineEdit::textChanged, this, &LocationEdit::highlight_location);
}

void LocationEdit::focusInEvent(QFocusEvent* event)
{
    QLineEdit::focusInEvent(event);
    highlight_location();
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

void LocationEdit::update_placeholder()
{
    if (Settings::the()->enable_search())
        setPlaceholderText(qstring_from_ak_string(
            MUST(String::formatted("Search with {} or enter web address",
                Settings::the()->search_engine().name))));
    else
        setPlaceholderText("Enter web address");
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

void LocationEdit::set_url(URL::URL const& url)
{
    m_url = url;
    if (m_url_is_hidden) {
        clear();
    } else {
        setText(qstring_from_ak_string(url.serialize()));
        setCursorPosition(0);
    }
}

}
