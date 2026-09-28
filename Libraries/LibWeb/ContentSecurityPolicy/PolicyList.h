/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Vector.h>
#include <LibWeb/ContentSecurityPolicy/Policy.h>

namespace Web::ContentSecurityPolicy {

// https://w3c.github.io/webappsec-csp/#csp-list
class PolicyList {
public:
    PolicyList() = default;
    explicit PolicyList(Vector<SerializedPolicy> const&);

    [[nodiscard]] Vector<NonnullRefPtr<Policy const>> const& policies() const { return m_policies; }

    [[nodiscard]] bool contains_header_delivered_policy() const;
    [[nodiscard]] HTML::SandboxingFlagSet csp_derived_sandboxing_flags() const;

    void enforce_policy(NonnullRefPtr<Policy const>);

    [[nodiscard]] Vector<SerializedPolicy> serialize() const;

private:
    Vector<NonnullRefPtr<Policy const>> m_policies;
};

}
