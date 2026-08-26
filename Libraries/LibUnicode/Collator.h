/*
 * Copyright (c) 2024-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
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
    struct Match {
        size_t start;
        size_t end;
    };

    class SubstringSearcher {
    public:
        virtual ~SubstringSearcher() = default;
        virtual Optional<Match> find_from(size_t start_offset) = 0;
    };

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

    // https://wicg.github.io/scroll-to-text-fragment/#finding-ranges-in-a-document
    // The string search must be performed using a base character comparison, or the primary
    // level, as defined in UTS10.
    virtual NonnullOwnPtr<SubstringSearcher> create_substring_searcher(Utf16View const& haystack, Utf16View const& needle) const = 0;

    virtual Sensitivity sensitivity() const = 0;
    virtual bool ignore_punctuation() const = 0;

protected:
    Collator() = default;
};

}
