/*
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/String.h>
#include <LibWebView/Forward.h>

#include <QCompleter>
#include <QListView>
#include <QStringListModel>

namespace Ladybird {

class Autocomplete final : public QCompleter {
    Q_OBJECT

public:
    explicit Autocomplete(QWidget* parent);

    void query_autocomplete_engine(String);

private:
    NonnullOwnPtr<WebView::Autocomplete> m_autocomplete;

    QStringListModel* m_model { nullptr };
    QListView* m_popup { nullptr };
};

}
