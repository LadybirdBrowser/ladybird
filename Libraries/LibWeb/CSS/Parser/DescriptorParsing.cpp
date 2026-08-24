/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Descriptor.h>
#include <LibWeb/CSS/DescriptorNameAndID.h>
#include <LibWeb/CSS/Parser/Parser.h>

namespace Web::CSS::Parser {

Optional<Descriptor> Parser::convert_to_descriptor(AtRuleID at_rule_id, Declaration const& declaration)
{
    auto descriptor_name_and_id = DescriptorNameAndID::from_name(at_rule_id, declaration.name);
    if (!descriptor_name_and_id.has_value() || !declaration.parsed_value)
        return {};

    return Descriptor { descriptor_name_and_id.value(), NonnullRefPtr<StyleValue const> { *declaration.parsed_value } };
}

}
