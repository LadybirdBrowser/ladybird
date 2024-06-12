/*
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "AutoComplete.h"
#include <AK/OwnPtr.h>
#include <QLineEdit>

namespace Ladybird {

class LocationEdit final : public QLineEdit {
    Q_OBJECT
public:
    explicit LocationEdit(QWidget*);

    URL::URL url() const { return m_url; }
    void set_url(URL::URL const&);

    bool url_is_hidden() const { return m_url_is_hidden; }
    void set_url_is_hidden(bool url_is_hidden) { m_url_is_hidden = url_is_hidden; }

private:
    virtual void focusInEvent(QFocusEvent* event) override;
    virtual void focusOutEvent(QFocusEvent* event) override;

    void update_placeholder();
    void highlight_location();
    AK::OwnPtr<AutoComplete> m_autocomplete;

    URL::URL m_url;
    bool m_url_is_hidden { false };
};

}
