/*
 * Copyright (c) 2024, Noah Bright <noah.bright.1@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>
#include <AK/String.h>
#include <LibURL/Origin.h>

// https://w3c.github.io/webappsec-csp/#framework-policy
namespace Web::HTML {

enum class PolicyDisposition {
    Enforce,
    Report
};

enum class PolicySource {
    Head,
    Meta
};

// https://w3c.github.io/webappsec-csp/#content-security-policy-object
struct Policy {
    // https://w3c.github.io/webappsec-csp/#directives
    OrderedHashMap<String, Optional<HashTable<String>>> directive_set;
    PolicyDisposition disposition;
    PolicySource source;
    URL::Origin self_origin;
};

using CSPList = Vector<Policy>;

}
