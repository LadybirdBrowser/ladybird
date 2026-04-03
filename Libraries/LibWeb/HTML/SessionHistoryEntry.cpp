/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/VM.h>
#include <LibWeb/Crypto/Crypto.h>
#include <LibWeb/HTML/DocumentState.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWeb/HTML/StructuredSerialize.h>

namespace Web::HTML {

NonnullRefPtr<SessionHistoryEntry> SessionHistoryEntry::create()
{
    return adopt_ref(*new SessionHistoryEntry());
}

SessionHistoryEntry::~SessionHistoryEntry() = default;

RefPtr<DocumentState> SessionHistoryEntry::document_state() const { return m_document_state; }
void SessionHistoryEntry::set_document_state(RefPtr<DocumentState> document_state) { m_document_state = move(document_state); }

SessionHistoryEntry::SessionHistoryEntry()
    : m_classic_history_api_state(MUST(structured_serialize_for_storage(JS::VM::the(), JS::js_null())))
    , m_navigation_api_state(MUST(structured_serialize_for_storage(JS::VM::the(), JS::js_undefined())))
    , m_navigation_api_key(Crypto::generate_random_uuid())
    , m_navigation_api_id(Crypto::generate_random_uuid())
{
}

}
