/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/kmalloc.h>

#include <QLabel>

namespace Ladybird {

// A one-line label that takes the width it is given, and elides its text to fit.
class ElidedLabel final : public QLabel {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit ElidedLabel(QString text, Qt::TextElideMode elide_mode, QWidget* parent = nullptr);

    virtual QSize sizeHint() const override;
    virtual QSize minimumSizeHint() const override;

protected:
    virtual void paintEvent(QPaintEvent*) override;

private:
    Qt::TextElideMode m_elide_mode { Qt::ElideRight };
};

}
