/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/kmalloc.h>
#include <UI/Qt/Popover.h>

#include <QObject>
#include <QPointer>

class QEvent;

namespace Ladybird {

class CrashReportNotifier final : public QObject {
public:
    AK_ALLOC_WITH_KMALLOC;

    void offer(ByteString report_name);
    void show_pending();

private:
    virtual bool eventFilter(QObject*, QEvent*) override;

    Optional<ByteString> m_pending_report_name;
    QPointer<MessagePopover> m_popover;
    QPointer<QWidget> m_anchor;
};

}
