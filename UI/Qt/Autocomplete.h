/*
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <LibWebView/Forward.h>

#include <QCompleter>
#include <QListView>
#include <QStandardItemModel>

namespace Ladybird {

class Autocomplete final : public QCompleter {
    Q_OBJECT

public:
    explicit Autocomplete(QWidget* parent);

    void query_autocomplete_engine(String);
    void notify_omnibox_interaction();
    void record_committed_input(String const&);
    void record_navigation(String const&, Optional<String> title = {});
    void update_navigation_title(String const&, String const&);
    void record_bookmark(String const&);

private:
    void clear_popup_selection();

    NonnullOwnPtr<WebView::Autocomplete> m_autocomplete;

    QStandardItemModel* m_model { nullptr };
    QListView* m_popup { nullptr };
};

}
