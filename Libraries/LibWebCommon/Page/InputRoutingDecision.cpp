/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <LibWebCommon/Page/InputRoutingDecision.h>

namespace Web {

// NB: Rust has the decisions without their strings, and makes each string from the name.
static consteval bool is_kebab_case_of(StringView string, StringView name)
{
    size_t string_index = 0;
    for (size_t name_index = 0; name_index < name.length(); ++name_index) {
        auto character = name[name_index];
        auto is_upper_case = character >= 'A' && character <= 'Z';
        if (is_upper_case && name_index > 0 && (string_index >= string.length() || string[string_index++] != '-'))
            return false;
        auto lower_case_character = is_upper_case ? static_cast<char>(character - 'A' + 'a') : character;
        if (string_index >= string.length() || string[string_index++] != lower_case_character)
            return false;
    }
    return string_index == string.length();
}

#define __ENUMERATE_INPUT_ROUTING_DECISION(name, string) static_assert(is_kebab_case_of(string##sv, #name##sv));
ENUMERATE_INPUT_ROUTING_DECISIONS(__ENUMERATE_INPUT_ROUTING_DECISION)
#undef __ENUMERATE_INPUT_ROUTING_DECISION

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
