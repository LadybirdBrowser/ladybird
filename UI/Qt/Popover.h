/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/kmalloc.h>

#include <QFrame>

namespace Ladybird {

class MessagePopover final : public QFrame {
public:
    AK_ALLOC_WITH_KMALLOC;

    MessagePopover(QWidget* parent, int width, QString const& title, QString const& body, QString const& dismiss_text, QString const& accept_text);

    void update_chrome_style(QPalette const&);

    Function<void()> on_accept;
};

void move_popover_below(QWidget& popover, QWidget& anchor);

}
