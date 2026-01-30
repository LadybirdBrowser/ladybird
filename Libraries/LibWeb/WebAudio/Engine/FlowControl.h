/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibCore/System.h>
#include <errno.h>

namespace Web::WebAudio::Render {

enum class DrainNotifyFDResult : u8 {
    Drained,
    Closed,
    Broken,
};

// Drain a nonblocking notify fd to coalesce signals.
// Returns Closed if the write end was closed (read returned 0).
// Returns Broken on any other read error (other than EAGAIN/EWOULDBLOCK).
inline DrainNotifyFDResult drain_nonblocking_notify_fd(int fd)
{
    if (fd < 0)
        return DrainNotifyFDResult::Broken;

    Array<u8, 64> buffer;
    while (true) {
        auto nread_or_error = Core::System::read(fd, buffer.span());
        if (nread_or_error.is_error()) {
            auto const& error = nread_or_error.error();
            if (error.is_errno() && (error.code() == EAGAIN || error.code() == EWOULDBLOCK))
                return DrainNotifyFDResult::Drained;
            return DrainNotifyFDResult::Broken;
        }
        if (nread_or_error.value() == 0)
            return DrainNotifyFDResult::Closed;
    }
}

enum class TransactionalPublishOutcome : u8 {
    NoPublishNeeded,
    Published,
    RetryLater,
    Failed,
};

// Publish a bindings list in a way that avoids sending empty or partial lists for
// resources that are expected to exist.
//
// - should_publish: caller decided something changed or initial publish is needed.
// - expected_nonempty: the graph/resources say there should be entries.
// - require_complete_set: if true, skipped_any forces RetryLater to avoid partial publish.
// - skipped_any: builder had to skip entries (e.g. missing state or fd clone failure).
//
// publish_callback must return true on success.
template<typename Descriptor, typename PublishCallback>
inline TransactionalPublishOutcome transactional_publish_bindings(
    bool should_publish,
    bool expected_nonempty,
    bool require_complete_set,
    Vector<Descriptor>&& descriptors,
    bool skipped_any,
    PublishCallback publish_callback)
{
    if (!should_publish)
        return TransactionalPublishOutcome::NoPublishNeeded;

    if (expected_nonempty) {
        if (descriptors.is_empty())
            return TransactionalPublishOutcome::RetryLater;
        if (require_complete_set && skipped_any)
            return TransactionalPublishOutcome::RetryLater;
    }

    bool ok = publish_callback(move(descriptors));
    if (!ok)
        return TransactionalPublishOutcome::Failed;

    return TransactionalPublishOutcome::Published;
}

}
