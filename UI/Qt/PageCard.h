/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>

#include <QFrame>
#include <QPointer>
#include <QString>

namespace Ladybird {

class PageCard final : public QFrame {
public:
    AK_ALLOC_WITH_KMALLOC;

    enum class ButtonRole : u8 {
        Secondary,
        Primary,
    };

    struct Button {
        QString label;
        ButtonRole role { ButtonRole::Secondary };
        Function<void()> on_click;
    };

    PageCard(QWidget& parent, QString const& title, QString const& text, Vector<Button>);

    void reposition(QWidget& page, int top_offset);

private:
    virtual bool event(QEvent*) override;
    virtual void showEvent(QShowEvent*) override;

    void update_chrome_style();

    QPointer<QWidget> m_page;
};

}
