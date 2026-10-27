/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/kmalloc.h>

#include <QFrame>

class QLabel;

namespace Ladybird {

// After Ladybird quit unexpectedly, a card over the top of the page offers to restore the tabs that were open.
class RestoreSessionCard final : public QFrame {
public:
    AK_ALLOC_WITH_KMALLOC;

    RestoreSessionCard(QWidget& parent, QWidget& page, size_t tab_count);

    void reposition();

    Function<void()> on_restore;

private:
    virtual bool event(QEvent*) override;
    virtual void showEvent(QShowEvent*) override;

    void update_chrome_style();

    QWidget& m_page;
};

}
