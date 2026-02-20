/*
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/Autocomplete.h>
#include <UI/Qt/Autocomplete.h>
#include <UI/Qt/StringUtils.h>

#include <QApplication>
#include <QItemSelectionModel>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QTimer>

namespace Ladybird {

static constexpr int max_visible_suggestion_count = 20;

struct SuggestionParts {
    QString prefix;
    QString host;
    QString suffix;
    bool has_host { false };
};

enum SuggestionDataRole {
    CompletionTextRole = Qt::UserRole + 1,
    TitleTextRole,
    IsRebuildPlaceholderRole,
};

static bool is_likely_host(QString const& candidate)
{
    return candidate.contains('.') || candidate.contains(':') || candidate.compare("localhost", Qt::CaseInsensitive) == 0;
}

static SuggestionParts split_suggestion(QString const& suggestion)
{
    auto scheme_separator_index = suggestion.indexOf("://");
    int host_start = 0;

    QString prefix;
    if (scheme_separator_index >= 0) {
        host_start = scheme_separator_index + 3;
        prefix = suggestion.left(host_start);
    }

    auto host_end = suggestion.indexOf('/', host_start);
    if (host_end < 0)
        host_end = suggestion.length();
    if (host_end <= host_start)
        return {};

    auto host = suggestion.mid(host_start, host_end - host_start);
    if (!is_likely_host(host))
        return {};

    return {
        .prefix = move(prefix),
        .host = move(host),
        .suffix = suggestion.mid(host_end),
        .has_host = true,
    };
}

static QString const& rebuild_placeholder_text()
{
    static auto const text = QStringLiteral("Rebuilding local suggestion index...");
    return text;
}

static QString const& suggestion_title_separator()
{
    static auto const separator = QStringLiteral(" \u2014 ");
    return separator;
}

class SuggestionDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    virtual void paint(QPainter* painter, QStyleOptionViewItem const& option, QModelIndex const& index) const override
    {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);

        auto suggestion_text = index.data(CompletionTextRole).toString();
        if (suggestion_text.isEmpty())
            suggestion_text = opt.text;
        auto suggestion_title = index.data(TitleTextRole).toString();
        auto is_rebuild_placeholder = index.data(IsRebuildPlaceholderRole).toBool() || suggestion_text == rebuild_placeholder_text();
        opt.text.clear();

        auto* style = opt.widget ? opt.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

        auto text_rect = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, opt.widget);
        auto text_color = opt.state & QStyle::State_Selected ? opt.palette.color(QPalette::HighlightedText) : opt.palette.color(QPalette::Text);
        auto secondary_text_color = opt.palette.color(QPalette::PlaceholderText);

        QFont normal_font = opt.font;
        QFont bold_font = opt.font;
        bold_font.setBold(true);

        QFontMetrics normal_metrics(normal_font);
        QFontMetrics bold_metrics(bold_font);

        painter->save();
        painter->setClipRect(text_rect);
        painter->setPen(text_color);

        if (is_rebuild_placeholder) {
            QFont italic_font = opt.font;
            italic_font.setItalic(true);
            QFontMetrics italic_metrics(italic_font);

            painter->setFont(italic_font);
            painter->drawText(text_rect, opt.displayAlignment, italic_metrics.elidedText(suggestion_text, Qt::ElideRight, text_rect.width()));
            painter->restore();
            return;
        }

        auto draw_suffix_title = [&](int x) {
            if (suggestion_title.isEmpty())
                return;

            auto separator = suggestion_title_separator();
            auto separator_width = normal_metrics.horizontalAdvance(separator);
            auto available_width = text_rect.right() - x + 1;
            if (separator_width >= available_width)
                return;

            painter->setPen(secondary_text_color);
            painter->setFont(normal_font);
            auto baseline = text_rect.y() + (text_rect.height() + normal_metrics.ascent() - normal_metrics.descent()) / 2;
            painter->drawText(x, baseline, separator);
            x += separator_width;

            auto title_width = text_rect.right() - x + 1;
            auto elided_title = normal_metrics.elidedText(suggestion_title, Qt::ElideRight, title_width);
            painter->drawText(x, baseline, elided_title);
            painter->setPen(text_color);
        };

        auto parts = split_suggestion(suggestion_text);
        if (!parts.has_host) {
            auto primary_width = normal_metrics.horizontalAdvance(suggestion_text);
            if (primary_width > text_rect.width()) {
                painter->setFont(normal_font);
                painter->drawText(text_rect, opt.displayAlignment, normal_metrics.elidedText(suggestion_text, Qt::ElideRight, text_rect.width()));
                painter->restore();
                return;
            }

            painter->setFont(normal_font);
            auto baseline = text_rect.y() + (text_rect.height() + normal_metrics.ascent() - normal_metrics.descent()) / 2;
            auto x = text_rect.x();
            painter->drawText(x, baseline, suggestion_text);
            x += primary_width;
            draw_suffix_title(x);
            painter->restore();
            return;
        }

        auto primary_width = normal_metrics.horizontalAdvance(parts.prefix)
            + bold_metrics.horizontalAdvance(parts.host)
            + normal_metrics.horizontalAdvance(parts.suffix);

        if (primary_width > text_rect.width()) {
            painter->setFont(normal_font);
            painter->drawText(text_rect, opt.displayAlignment, normal_metrics.elidedText(suggestion_text, Qt::ElideRight, text_rect.width()));
            painter->restore();
            return;
        }

        auto baseline = text_rect.y() + (text_rect.height() + normal_metrics.ascent() - normal_metrics.descent()) / 2;
        auto x = text_rect.x();

        painter->setFont(normal_font);
        painter->drawText(x, baseline, parts.prefix);
        x += normal_metrics.horizontalAdvance(parts.prefix);

        painter->setFont(bold_font);
        painter->drawText(x, baseline, parts.host);
        x += bold_metrics.horizontalAdvance(parts.host);

        painter->setFont(normal_font);
        painter->drawText(x, baseline, parts.suffix);
        x += normal_metrics.horizontalAdvance(parts.suffix);
        draw_suffix_title(x);

        painter->restore();
    }
};

Autocomplete::Autocomplete(QWidget* parent)
    : QCompleter(parent)
    , m_autocomplete(make<WebView::Autocomplete>())
    , m_model(new QStandardItemModel(this))
    , m_popup(new QListView(parent))
{
    m_autocomplete->on_suggestions_query_complete = [this](auto const& suggestions) {
        m_model->clear();

        if (suggestions.is_empty()) {
            clear_popup_selection();
        } else {
            auto suggestion_count = suggestions.size();
            if (suggestion_count > static_cast<size_t>(max_visible_suggestion_count))
                suggestion_count = static_cast<size_t>(max_visible_suggestion_count);
            for (size_t index = 0; index < suggestion_count; ++index) {
                auto const& suggestion = suggestions[index];
                auto completion_text = qstring_from_ak_string(suggestion.text);
                auto title_text = suggestion.title.has_value() ? qstring_from_ak_string(*suggestion.title) : QString {};
                auto is_rebuild_placeholder = completion_text == rebuild_placeholder_text();

                auto* item = new QStandardItem(completion_text);
                item->setData(completion_text, CompletionTextRole);
                item->setData(title_text, TitleTextRole);
                item->setData(is_rebuild_placeholder, IsRebuildPlaceholderRole);
                m_model->appendRow(item);
            }

            complete();
            clear_popup_selection();
            m_popup->scrollToTop();

            // QCompleter may re-apply a current row while showing the popup.
            // Clear it on the next event loop turn so no item appears preselected.
            QTimer::singleShot(0, m_popup, [this] {
                clear_popup_selection();
                m_popup->scrollToTop();
            });
        }
    };

    setCompletionMode(QCompleter::UnfilteredPopupCompletion);
    setModelSorting(QCompleter::UnsortedModel);
    setMaxVisibleItems(max_visible_suggestion_count);
    setModel(m_model);
    setCompletionRole(CompletionTextRole);
    setPopup(m_popup);
    m_popup->setAutoScroll(false);
    m_popup->setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
    m_popup->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_popup->setItemDelegate(new SuggestionDelegate(m_popup));
}

void Autocomplete::clear_popup_selection()
{
    if (auto* selection_model = m_popup->selectionModel()) {
        selection_model->clearSelection();
        selection_model->setCurrentIndex({}, QItemSelectionModel::NoUpdate);
    }

    m_popup->setCurrentIndex({});
}

void Autocomplete::query_autocomplete_engine(String search_string)
{
    m_autocomplete->query_autocomplete_engine(move(search_string));
}

void Autocomplete::notify_omnibox_interaction()
{
    m_autocomplete->notify_omnibox_interaction();
}

void Autocomplete::record_committed_input(String const& text)
{
    m_autocomplete->record_committed_input(text);
}

void Autocomplete::record_navigation(String const& text, Optional<String> title)
{
    m_autocomplete->record_navigation(text, move(title));
}

void Autocomplete::update_navigation_title(String const& text, String const& title)
{
    m_autocomplete->update_navigation_title(text, move(title));
}

void Autocomplete::record_bookmark(String const& text)
{
    m_autocomplete->record_bookmark(text);
}

}
