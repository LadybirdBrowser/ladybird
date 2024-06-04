/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Types.h>
#include <LibCore/Platform/ProcessInfo.h>

#if defined(AK_OS_MACH)
#    include <LibCore/MachPort.h>
#endif

namespace WebView {

enum class ProcessType {
    Chrome,
    WebContent,
    WebWorker,
    RequestServer,
    ImageDecoder,
};

struct ProcessInfo : public Core::Platform::ProcessInfo {
    using Core::Platform::ProcessInfo::ProcessInfo;

    ProcessInfo(ProcessType type, pid_t pid)
        : Core::Platform::ProcessInfo(pid)
        , type(type)
    {
    }

    ProcessType type { ProcessType::WebContent };
    Optional<String> title;
};

}
