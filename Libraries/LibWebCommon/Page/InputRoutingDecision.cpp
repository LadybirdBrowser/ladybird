/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <LibWebCommon/Page/InputRoutingDecision.h>

namespace Web {

StringView input_routing_decision_name(InputRoutingDecision decision)
{
    switch (decision) {
#define __ENUMERATE_INPUT_ROUTING_DECISION(name, string) \
    case InputRoutingDecision::name:                     \
        return string##sv;
        ENUMERATE_INPUT_ROUTING_DECISIONS(__ENUMERATE_INPUT_ROUTING_DECISION)
#undef __ENUMERATE_INPUT_ROUTING_DECISION
    }
    VERIFY_NOT_REACHED();
}

}
