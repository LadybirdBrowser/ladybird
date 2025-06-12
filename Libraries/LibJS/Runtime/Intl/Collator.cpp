/*
 * Copyright (c) 2022-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Intl/Collator.h>

namespace JS::Intl {

GC_DEFINE_ALLOCATOR(Collator);

// 10 Collator Objects, https://tc39.es/ecma402/#collator-objects
Collator::Collator(Object& prototype)
    : IntlObject(ConstructWithPrototypeTag::Tag, prototype)
{
}

void Collator::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_bound_compare);
}

// 10.2.3 Internal slots, https://tc39.es/ecma402/#sec-intl-collator-internal-slots
ReadonlySpan<StringView> Collator::relevant_extension_keys() const
{
    // The value of the [[RelevantExtensionKeys]] internal slot is a List that must include the element "co", may include any or all of the elements "kf" and "kn", and must not include any other elements.
    static constexpr AK::Array keys { "co"sv, "kf"sv, "kn"sv };
    return keys;
}

// 10.2.3 Internal slots, https://tc39.es/ecma402/#sec-intl-collator-internal-slots
ReadonlySpan<ResolutionOptionDescriptor> Collator::resolution_option_descriptors(VM& vm) const
{
    // The value of the [[ResolutionOptionDescriptors]] internal slot is « { [[Key]]: "co", [[Property]]: "collation" }, { [[Key]]: "kn", [[Property]]: "numeric", [[Type]]: boolean }, { [[Key]]: "kf", [[Property]]: "caseFirst", [[Values]]: « "upper", "lower", "false" » } ».
    static constexpr AK::Array case_first_values { "upper"sv, "lower"sv, "false"sv };

    static auto descriptors = to_array<ResolutionOptionDescriptor>({
        { .key = "co"sv, .property = vm.names.collation },
        { .key = "kn"sv, .property = vm.names.numeric, .type = OptionType::Boolean },
        { .key = "kf"sv, .property = vm.names.caseFirst, .values = case_first_values },
    });

    return descriptors;
}

}
