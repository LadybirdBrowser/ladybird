/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2024-2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Export.h>
#include <LibWebCommon/Forward.h>

namespace Web::HTML {

// Steps 6 to 9 of BroadcastChannel's postMessage(), for a message that another process posted.
WEB_API void deliver_broadcast_channel_message_locally(BroadcastChannelMessage);

}
