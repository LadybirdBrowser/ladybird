/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/kmalloc.h>

#include <QWidget>

namespace Ladybird {

// The bar every window shows below its toolbar while the browser runs without the sandbox. It can't be dismissed, as
// nothing in the browser can turn the sandbox back on.
class SandboxOffBar final : public QWidget {
    Q_OBJECT

public:
    AK_ALLOC_WITH_KMALLOC;

    explicit SandboxOffBar(QWidget* parent = nullptr);

private:
    virtual bool event(QEvent*) override;
    virtual void paintEvent(QPaintEvent*) override;
};

}
