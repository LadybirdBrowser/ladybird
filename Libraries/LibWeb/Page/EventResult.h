/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web {

enum class EventResult {
    // The event is allowed to continue. It was not cancelled by the page, nor handled explicitly by the WebContent
    // process. The UI process is allowed to further process the event.
    Accepted,

    // The event was accepted, and was handled explicitly by the WebContent process. The UI process should not further
    // process the event.
    Handled,

    // The event was not accepted by the WebContent process (e.g. because the document is no longer active, or a
    // drag-and-drop is active).
    Dropped,

    // The event was cancelled by the page (e.g. by way of e.preventDefault()).
    Cancelled,
};

}
