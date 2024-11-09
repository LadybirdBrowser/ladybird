/*
 * Copyright (c) 2023, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/EventLoopImplementationQtEventTarget.h>

namespace Ladybird {

bool EventLoopImplementationQtEventTarget::event(QEvent* event)
{
    return EventLoopManagerQt::event_target_received_event({}, event);
}

}
