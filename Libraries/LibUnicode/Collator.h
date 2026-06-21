/*
 * Copyright (c) 2024-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>

namespace Unicode {

enum class Usage {
    Sort,
    Search,
};
Usage usage_from_string(Utf16View);
Utf16String usage_to_string(Usage);

enum class Sensitivity {
    Base,
    Accent,
    Case,
    Variant,
};
Sensitivity sensitivity_from_string(Utf16View);
Utf16String sensitivity_to_string(Sensitivity);

enum class CaseFirst {
    Upper,
    Lower,
    False,
};
CaseFirst case_first_from_string(Utf16View);
Utf16String case_first_to_string(CaseFirst);

class Collator {
public:
    static NonnullOwnPtr<Collator> create(
        Utf16View locale,
        Usage,
        Utf16View collation,
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
    virtual Order compare(Utf16View const&, Utf16View const&) const = 0;

    virtual Sensitivity sensitivity() const = 0;
    virtual bool ignore_punctuation() const = 0;

protected:
    Collator() = default;
};

}
