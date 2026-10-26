/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Web::ContentSecurityPolicy {

// The directive of a violation a navigation found of a policy of its source, which another process hosts.
enum class RemoteViolationDirective : u8 {
    FrameSrc,
    ObjectSrc,
    FormAction,
};

}

#include <LibWebCommon/ContentSecurityPolicy/RemoteViolationDirectiveCodecs.h>
