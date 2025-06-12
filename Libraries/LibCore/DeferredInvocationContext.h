/*
 * Copyright (c) 2018-2020, sin-ack <sin-ack@protonmail.com>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCore/EventReceiver.h>

namespace Core {

class DeferredInvocationContext final : public Core::EventReceiver {
    C_OBJECT(DeferredInvocationContext)
private:
    DeferredInvocationContext() = default;
};

}
