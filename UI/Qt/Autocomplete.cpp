/*
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/Autocomplete.h>
#include <UI/Qt/Autocomplete.h>
#include <UI/Qt/StringUtils.h>

namespace Ladybird {

Autocomplete::Autocomplete(QWidget* parent)
    : QCompleter(parent)
    , m_autocomplete(make<WebView::Autocomplete>())
    , m_model(new QStringListModel(this))
    , m_popup(new QListView(parent))
{
    m_autocomplete->on_autocomplete_query_complete = [this](auto const& suggestions) {
        if (suggestions.is_empty()) {
            m_model->setStringList({});
        } else {
            QStringList list;
            for (auto const& suggestion : suggestions)
                list.append(qstring_from_ak_string(suggestion));

            m_model->setStringList(list);
            complete();
        }
    };

    setCompletionMode(QCompleter::UnfilteredPopupCompletion);
    setModel(m_model);
    setPopup(m_popup);
}

void Autocomplete::query_autocomplete_engine(String search_string)
{
    m_autocomplete->query_autocomplete_engine(move(search_string));
}

}
