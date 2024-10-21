/*
 * Copyright (c) 2024, Benjamin Bjerken <beuss-git@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Heap/GCPtr.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

#define CONSTRAINT_VALIDATION_IMPL()                                                                           \
    JS::NonnullGCPtr<ValidityState const> validity() const { return ConstraintValidation::validity(*this); }   \
    bool will_validate() const { return ConstraintValidation::will_validate(*this); }                          \
    void set_custom_validity(String const& error) { ConstraintValidation::set_custom_validity(error, *this); } \
    WebIDL::ExceptionOr<bool> check_validity() { return ConstraintValidation::check_validity(*this); }         \
    WebIDL::ExceptionOr<bool> report_validity() { return ConstraintValidation::report_validity(*this); }       \
    String validation_message() const { return ConstraintValidation::validation_message(*this); }

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#the-constraint-validation-api
class ConstraintValidation {
public:
    virtual ~ConstraintValidation() = default;

    JS::NonnullGCPtr<ValidityState const> validity(DOM::Element const&) const;
    bool will_validate(DOM::Element const&) const;
    void set_custom_validity(String const&, DOM::Element const&);
    WebIDL::ExceptionOr<bool> check_validity(DOM::Element const&);
    WebIDL::ExceptionOr<bool> report_validity(DOM::Element const&);
    String validation_message(DOM::Element const& element) const;

    friend class ValidityState;

protected:
    bool is_candidate_for_constraint_validation(DOM::Element const& element) const;
    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#barred-from-constraint-validation
    virtual bool is_barred_from_constraint_validation() const { return false; }

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#constraints
    virtual bool is_value_missing() const { return false; }
    virtual bool is_type_mismatch() const { return false; }
    virtual bool is_pattern_mismatch() const { return false; }
    virtual bool is_too_long() const { return false; }
    virtual bool is_too_short() const { return false; }
    virtual bool is_range_underflow() const { return false; }
    virtual bool is_range_overflow() const { return false; }
    virtual bool is_step_mismatch() const { return false; }
    virtual bool is_bad_input() const { return false; }
    bool has_custom_error() const { return !m_custom_validity_error_message.is_empty(); }
    bool is_valid() const
    {
        return !is_value_missing()
            && !is_type_mismatch()
            && !is_pattern_mismatch()
            && !is_too_long()
            && !is_too_short()
            && !is_range_underflow()
            && !is_range_underflow()
            && !is_step_mismatch()
            && !is_bad_input()
            && !has_custom_error();
    }

    String custom_validity_error_message() const { return m_custom_validity_error_message; }

    mutable JS::GCPtr<ValidityState const> m_validity;

private:
    String m_custom_validity_error_message;
};
}
