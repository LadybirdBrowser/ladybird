/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/StringView.h>

namespace Unicode {

enum class Usage {
    Sort,
    Search,
};
Usage usage_from_string(StringView);
StringView usage_to_string(Usage);

enum class Sensitivity {
    Base,
    Accent,
    Case,
    Variant,
};
Sensitivity sensitivity_from_string(StringView);
StringView sensitivity_to_string(Sensitivity);

enum class CaseFirst {
    Upper,
    Lower,
    False,
};
CaseFirst case_first_from_string(StringView);
StringView case_first_to_string(CaseFirst);

class Collator {
public:
    static NonnullOwnPtr<Collator> create(
        StringView locale,
        Usage,
        StringView collation,
        Optional<Sensitivity>,
        CaseFirst,
        bool numeric,
        Optional<bool> ignore_punctuation);

    virtual ~Collator() = default;

    enum class Order {
        Before,
        Equal,
        After,
    };
    virtual Order compare(StringView, StringView) const = 0;

    virtual Sensitivity sensitivity() const = 0;
    virtual bool ignore_punctuation() const = 0;

protected:
    Collator() = default;
};

}
