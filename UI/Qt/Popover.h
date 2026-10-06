/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/kmalloc.h>

#include <QWidget>

class QFrame;
class QHideEvent;

namespace Ladybird {

class Popover : public QWidget {
public:
    AK_ALLOC_WITH_KMALLOC;

    QFrame& card() { return *m_card; }

protected:
    explicit Popover(QWidget* parent);

private:
    QFrame* m_card { nullptr };
};

class MessagePopover final : public Popover {
public:
    AK_ALLOC_WITH_KMALLOC;

    MessagePopover(QWidget* parent, int width, QString const& title, QString const& body, QString const& dismiss_text, QString const& accept_text);

    void update_chrome_style(QPalette const&);

    Function<void()> on_accept;
    Function<void()> on_dismiss;

private:
    virtual void hideEvent(QHideEvent*) override;

    bool m_is_accepted { false };
};

void move_popover_below(Popover&, QWidget& anchor);

}
