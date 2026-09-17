/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <QObject>
#include <QTimer>

namespace Ladybird {

class FullscreenDebounce {
public:
    void start(QObject& context, int milliseconds)
    {
        m_active = true;
        QTimer::singleShot(milliseconds, &context, [this] {
            m_active = false;
        });
    }

    bool is_active() const { return m_active; }

private:
    bool m_active { false };
};

}
