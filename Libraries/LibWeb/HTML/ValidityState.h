/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/ValidityState.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/HTML/FormAssociatedElement.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#validitystate
class ValidityState final : public Bindings::Wrappable {
    WEB_WRAPPABLE(ValidityState, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(ValidityState);

public:
    [[nodiscard]] static GC::Ref<ValidityState> create(JS::Realm&, FormAssociatedElement const&);

    virtual ~ValidityState() override = default;

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-validitystate-valuemissing
    bool value_missing() const;
    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-validitystate-typemismatch
    bool type_mismatch() const;
    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-validitystate-patternmismatch
    bool pattern_mismatch() const;
    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-validitystate-toolong
    bool too_long() const;
    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-validitystate-tooshort
    bool too_short() const;
    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-validitystate-rangeunderflow
    bool range_underflow() const;
    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-validitystate-rangeoverflow
    bool range_overflow() const;
    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-validitystate-stepmismatch
    bool step_mismatch() const;
    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-validitystate-badinput
    bool bad_input() const;
    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-validitystate-customerror
    bool custom_error() const;
    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-validitystate-valid
    bool valid() const;

private:
    ValidityState(JS::Realm&, FormAssociatedElement const&);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    FormAssociatedElement const& m_control;
};

}
