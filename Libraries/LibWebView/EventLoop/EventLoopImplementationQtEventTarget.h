/*
 * Copyright (c) 2023, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebView/Forward.h>
#include <QEvent>
#include <QObject>

namespace WebView {

class WEBVIEW_API EventLoopImplementationQtEventTarget final : public QObject {
    Q_OBJECT

public:
    virtual bool event(QEvent* event) override;
};

}
