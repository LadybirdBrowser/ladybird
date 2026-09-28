/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Platform.h>

#if !defined(AK_OS_MACOS)
#    error "This file is only available on macOS"
#endif

#include <AK/Assertions.h>
#include <LibCore/Platform/ThreadQoS.h>
#include <errno.h>
#include <pthread/qos.h>
#include <sys/qos.h>

namespace Core::Platform {

static qos_class_t qos_class_for(ThreadQoS qos)
{
    switch (qos) {
    case ThreadQoS::UserInteractive:
        return QOS_CLASS_USER_INTERACTIVE;
    case ThreadQoS::UserInitiated:
        return QOS_CLASS_USER_INITIATED;
    case ThreadQoS::Default:
        return QOS_CLASS_DEFAULT;
    case ThreadQoS::Utility:
        return QOS_CLASS_UTILITY;
    case ThreadQoS::Background:
        return QOS_CLASS_BACKGROUND;
    }
    VERIFY_NOT_REACHED();
}

static ErrorOr<void> validate_relative_priority(int relative_priority)
{
    if (relative_priority > 0 || relative_priority < QOS_MIN_RELATIVE_PRIORITY)
        return Error::from_errno(EINVAL);
    return {};
}

ErrorOr<void> set_current_thread_qos(ThreadQoS qos, int relative_priority)
{
    TRY(validate_relative_priority(relative_priority));
    if (auto rc = pthread_set_qos_class_self_np(qos_class_for(qos), relative_priority); rc != 0)
        return Error::from_errno(rc);
    return {};
}

ErrorOr<void> apply_thread_qos_to_pthread_attributes(pthread_attr_t& attributes, ThreadQoS qos, int relative_priority)
{
    TRY(validate_relative_priority(relative_priority));
    if (auto rc = pthread_attr_set_qos_class_np(&attributes, qos_class_for(qos), relative_priority); rc != 0)
        return Error::from_errno(rc);
    return {};
}

}
