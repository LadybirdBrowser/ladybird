/*
 * Copyright (c) 2023, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <QEvent>
#include <QObject>

namespace WebView {

class EventLoopImplementationQtEventTarget final : public QObject {
    Q_OBJECT

public:
    virtual bool event(QEvent* event) override;
};

}
