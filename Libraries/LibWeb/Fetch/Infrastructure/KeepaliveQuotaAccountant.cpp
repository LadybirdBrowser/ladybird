/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Fetch/Infrastructure/KeepaliveQuotaAccountant.h>

namespace Web::Fetch::Infrastructure {

NonnullRefPtr<KeepaliveQuotaAccountant> KeepaliveQuotaAccountant::create()
{
    return adopt_ref(*new KeepaliveQuotaAccountant);
}

KeepaliveQuotaAccountant::Reservation KeepaliveQuotaAccountant::reserve(u64 byte_count)
{
    m_in_flight_byte_count.fetch_add(byte_count);
    return Reservation { *this, byte_count };
}

KeepaliveQuotaAccountant::Reservation::Reservation(NonnullRefPtr<KeepaliveQuotaAccountant> accountant, u64 byte_count)
    : m_accountant(move(accountant))
    , m_byte_count(byte_count)
{
}

KeepaliveQuotaAccountant::Reservation::Reservation(Reservation&& other)
    : m_accountant(move(other.m_accountant))
    , m_byte_count(exchange(other.m_byte_count, 0))
{
}

KeepaliveQuotaAccountant::Reservation& KeepaliveQuotaAccountant::Reservation::operator=(Reservation&& other)
{
    if (this != &other) {
        release();
        m_accountant = move(other.m_accountant);
        m_byte_count = exchange(other.m_byte_count, 0);
    }
    return *this;
}

KeepaliveQuotaAccountant::Reservation::~Reservation()
{
    release();
}

void KeepaliveQuotaAccountant::Reservation::release()
{
    if (!m_accountant)
        return;
    auto byte_count = exchange(m_byte_count, 0);
    auto previous_byte_count = m_accountant->m_in_flight_byte_count.fetch_sub(byte_count);
    VERIFY(previous_byte_count >= byte_count);
    m_accountant = nullptr;
}

}
