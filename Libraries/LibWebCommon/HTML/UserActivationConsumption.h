/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Web::HTML {

enum class UserActivationConsumption : u8 {
    // https://html.spec.whatwg.org/multipage/interaction.html#consume-user-activation
    Transient,
    // https://html.spec.whatwg.org/multipage/interaction.html#consume-history-action-user-activation
    HistoryAction,
};

}
