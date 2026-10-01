/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/kmalloc.h>

#include <QDialog>

namespace Ladybird {

// Asks about the crash reports that are still waiting for an answer, one after the other.
class CrashReportDialog final : public QDialog {
public:
    AK_ALLOC_WITH_KMALLOC;

    // Opens the dialog over the given window if any report is waiting for an answer.
    static void open_if_needed(QWidget& window);

private:
    explicit CrashReportDialog(QWidget& window);
};

}
