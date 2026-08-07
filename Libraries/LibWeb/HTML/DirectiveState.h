/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/String.h>
#include <LibWeb/HTML/CrossProcessId.h>

namespace Web::HTML {

// https://wicg.github.io/scroll-to-text-fragment/#the-fragment-directive
// Directive state holds the value of the fragment directive at the time the session history entry
// was created and is used to invoke directives, such as text highlighting, whenever the entry is
// traversed.
class DirectiveState final : public RefCounted<DirectiveState> {
public:
    static NonnullRefPtr<DirectiveState> create(CrossProcessId cross_process_id, Optional<String> value = {})
    {
        return adopt_ref(*new DirectiveState(cross_process_id, move(value)));
    }

    Optional<String> const& value() const { return m_value; }
    void set_value(Optional<String> value) { m_value = move(value); }

    CrossProcessId cross_process_id() const { return m_cross_process_id; }

private:
    DirectiveState(CrossProcessId cross_process_id, Optional<String> value)
        : m_value(move(value))
        , m_cross_process_id(cross_process_id)
    {
    }

    // value, the fragment directive ASCII string or null, initially null.
    Optional<String> m_value;

    // AD-HOC: Preserve object identity when entries cross process boundaries.
    CrossProcessId m_cross_process_id;
};

}
