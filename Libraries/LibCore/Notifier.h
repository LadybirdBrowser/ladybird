/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <LibCore/Event.h>
#include <LibCore/EventReceiver.h>
#include <LibCore/Export.h>
#include <pthread.h>

namespace Core {

enum class NotificationType : u8 {
    None = 0,
    Read = 1,
    Write = 2,
    HangUp = 4,
    Error = 8,
};

AK_ENUM_BITWISE_OPERATORS(NotificationType);

class CORE_API Notifier final : public EventReceiver {
    C_OBJECT(Notifier);

public:
    using Type = NotificationType;

    virtual ~Notifier() override;

    void set_enabled(bool);

    Function<void()> on_activation;

    void close();

    int fd() const { return m_fd; }
    Type type() const { return m_type; }
    void set_type(Type type);

    void event(Core::Event&) override;

    void set_owner_thread(pthread_t owner_thread) { m_owner_thread = owner_thread; }
    pthread_t owner_thread() const { return m_owner_thread; }

private:
    Notifier(int fd, Type type);

    pthread_t m_owner_thread {};
    int m_fd { -1 };
    Type m_type { Type::None };
    bool m_is_enabled { false };
};

}
