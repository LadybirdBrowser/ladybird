/*
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <LibWebView/Settings.h>

#include <QLineEdit>

namespace Ladybird {

class Autocomplete;

class LocationEdit final
    : public QLineEdit
    , public WebView::SettingsObserver {
    Q_OBJECT

public:
    explicit LocationEdit(QWidget*);

    URL::URL const& url() const { return m_url; }
    void set_url(URL::URL);

    bool url_is_hidden() const { return m_url_is_hidden; }
    void set_url_is_hidden(bool url_is_hidden) { m_url_is_hidden = url_is_hidden; }

private:
    virtual void focusInEvent(QFocusEvent* event) override;
    virtual void focusOutEvent(QFocusEvent* event) override;

    virtual void search_engine_changed() override;

    void update_placeholder();
    void highlight_location();

    Autocomplete* m_autocomplete { nullptr };

    URL::URL m_url;
    bool m_url_is_hidden { false };
};

}
