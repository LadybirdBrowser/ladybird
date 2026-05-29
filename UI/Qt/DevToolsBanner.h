/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

#include <QWidget>

class QLabel;

namespace Ladybird {

class DevToolsBanner final : public QWidget {
    Q_OBJECT

public:
    explicit DevToolsBanner(QWidget* parent = nullptr);

    void set_port(u16 port);

signals:
    void disable_requested();

private:
    virtual bool event(QEvent*) override;

    void update_chrome_style();

    QLabel* m_label { nullptr };
    bool m_is_updating_chrome_style { false };
};

}
