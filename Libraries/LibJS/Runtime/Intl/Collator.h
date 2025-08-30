/*
 * Copyright (c) 2022-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/StringView.h>
#include <LibJS/Runtime/Intl/CollatorCompareFunction.h>
#include <LibJS/Runtime/Intl/IntlObject.h>
#include <LibUnicode/Collator.h>

namespace JS::Intl {

class Collator final : public IntlObject {
    JS_OBJECT(Collator, IntlObject);
    GC_DECLARE_ALLOCATOR(Collator);

public:
    virtual ~Collator() override = default;

    virtual ReadonlySpan<StringView> relevant_extension_keys() const override;
    virtual ReadonlySpan<ResolutionOptionDescriptor> resolution_option_descriptors(VM&) const override;

    String const& locale() const { return m_locale; }
    void set_locale(String locale) { m_locale = move(locale); }

    Unicode::Usage usage() const { return m_usage; }
    void set_usage(StringView usage) { m_usage = Unicode::usage_from_string(usage); }
    StringView usage_string() const LIFETIME_BOUND { return Unicode::usage_to_string(m_usage); }

    Unicode::Sensitivity sensitivity() const { return m_sensitivity; }
    void set_sensitivity(Unicode::Sensitivity sensitivity) { m_sensitivity = sensitivity; }
    StringView sensitivity_string() const LIFETIME_BOUND { return Unicode::sensitivity_to_string(m_sensitivity); }

    Unicode::CaseFirst case_first() const { return m_case_first; }
    void set_case_first(StringView case_first) { m_case_first = Unicode::case_first_from_string(case_first); }
    StringView case_first_string() const LIFETIME_BOUND { return Unicode::case_first_to_string(m_case_first); }

    String const& collation() const { return m_collation; }
    void set_collation(String collation) { m_collation = move(collation); }

    bool ignore_punctuation() const { return m_ignore_punctuation; }
    void set_ignore_punctuation(bool ignore_punctuation) { m_ignore_punctuation = ignore_punctuation; }

    bool numeric() const { return m_numeric; }
    void set_numeric(bool numeric) { m_numeric = numeric; }

    CollatorCompareFunction* bound_compare() const { return m_bound_compare; }
    void set_bound_compare(CollatorCompareFunction* bound_compare) { m_bound_compare = bound_compare; }

    Unicode::Collator const& collator() const { return *m_collator; }
    void set_collator(NonnullOwnPtr<Unicode::Collator> collator) { m_collator = move(collator); }

private:
    explicit Collator(Object& prototype);

    virtual void visit_edges(Visitor&) override;

    String m_locale;                                                      // [[Locale]]
    Unicode::Usage m_usage { Unicode::Usage::Sort };                      // [[Usage]]
    Unicode::Sensitivity m_sensitivity { Unicode::Sensitivity::Variant }; // [[Sensitivity]]
    Unicode::CaseFirst m_case_first { Unicode::CaseFirst::False };        // [[CaseFirst]]
    String m_collation;                                                   // [[Collation]]
    bool m_ignore_punctuation { false };                                  // [[IgnorePunctuation]]
    bool m_numeric { false };                                             // [[Numeric]]
    GC::Ptr<CollatorCompareFunction> m_bound_compare;                     // [[BoundCompare]]

    // Non-standard. Stores the ICU collator for the Intl object's collation options.
    OwnPtr<Unicode::Collator> m_collator;
};

}
