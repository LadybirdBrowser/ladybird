/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <QToolBar>

namespace Ladybird {

class BookmarksBar final : public QToolBar {
    Q_OBJECT

public:
    explicit BookmarksBar(QWidget* parent = nullptr);

    void rebuild();
};

}
