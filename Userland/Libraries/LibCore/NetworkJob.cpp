/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibCore/NetworkJob.h>
#include <LibCore/NetworkResponse.h>

namespace Core {

NetworkJob::NetworkJob(Stream& output_stream)
    : m_output_stream(output_stream)
{
}

NetworkJob::~NetworkJob() = default;

void NetworkJob::did_finish(NonnullRefPtr<NetworkResponse>&& response)
{
    if (is_cancelled())
        return;

    // NOTE: We protect ourselves here, since the on_finish callback may otherwise
    //       trigger destruction of this job somehow.
    NonnullRefPtr<NetworkJob> protector(*this);

    m_response = move(response);
    dbgln_if(NETWORKJOB_DEBUG, "{} job did_finish", *this);
    VERIFY(on_finish);
    on_finish(true);
    shutdown(ShutdownMode::DetachFromSocket);
}

void NetworkJob::did_fail(Error error)
{
    if (is_cancelled())
        return;

    // NOTE: We protect ourselves here, since the on_finish callback may otherwise
    //       trigger destruction of this job somehow.
    NonnullRefPtr<NetworkJob> protector(*this);

    m_error = error;
    dbgln_if(NETWORKJOB_DEBUG, "{}{{{:p}}} job did_fail! error: {} ({})", class_name(), this, (unsigned)error, to_string(error));
    VERIFY(on_finish);
    on_finish(false);
    shutdown(ShutdownMode::DetachFromSocket);
}

void NetworkJob::did_progress(Optional<u64> total_size, u64 downloaded)
{
    if (is_cancelled())
        return;

    // NOTE: We protect ourselves here, since the callback may otherwise
    //       trigger destruction of this job somehow.
    NonnullRefPtr<NetworkJob> protector(*this);

    if (on_progress)
        on_progress(total_size, downloaded);
}

char const* to_string(NetworkJob::Error error)
{
    switch (error) {
    case NetworkJob::Error::ProtocolFailed:
        return "ProtocolFailed";
    case NetworkJob::Error::ConnectionFailed:
        return "ConnectionFailed";
    case NetworkJob::Error::TransmissionFailed:
        return "TransmissionFailed";
    case NetworkJob::Error::Cancelled:
        return "Cancelled";
    default:
        return "(Unknown error)";
    }
}

}
