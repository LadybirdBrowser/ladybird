/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/AtomicRefCounted.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <LibWeb/Export.h>

namespace Web::Fetch::Infrastructure {

// The sum of the body lengths of a fetch group's keepalive requests whose done flag is unset.
class WEB_API KeepaliveQuotaAccountant final : public AtomicRefCounted<KeepaliveQuotaAccountant> {
public:
    // Keeps its byte count in the accountant's sum until it is destroyed.
    class Reservation {
        AK_MAKE_NONCOPYABLE(Reservation);

    public:
        Reservation(Reservation&&);
        Reservation& operator=(Reservation&&);
        ~Reservation();

    private:
        friend class KeepaliveQuotaAccountant;

        Reservation(NonnullRefPtr<KeepaliveQuotaAccountant>, u64 byte_count);

        void release();

        RefPtr<KeepaliveQuotaAccountant> m_accountant;
        u64 m_byte_count { 0 };
    };

    [[nodiscard]] static NonnullRefPtr<KeepaliveQuotaAccountant> create();

    [[nodiscard]] Reservation reserve(u64 byte_count);

    [[nodiscard]] u64 in_flight_byte_count() const { return m_in_flight_byte_count.load(); }

private:
    KeepaliveQuotaAccountant() = default;

    Atomic<u64> m_in_flight_byte_count { 0 };
};

}
