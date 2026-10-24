/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWeb/StorageAPI/StorageAreaMirror.h>
#include <LibWebCommon/StorageAPI/StorageEndpoint.h>

using Web::StorageAPI::StorageAreaId;
using Web::StorageAPI::StorageAreaMirror;
using Web::StorageAPI::StorageEndpointType;

namespace {

struct Sent {
    enum class Kind : u8 {
        Connect,
        Disconnect,
        Set,
        Remove,
        Clear,
    };

    Kind kind;
    Optional<StorageAreaId> area_id;
    Utf16String key;
    Utf16String value;
    String url;
};

class FakeClient final : public Web::StorageAPI::StorageAreaMirrorClient {
public:
    Vector<Sent> sent;
    // What the next connect answers with.
    Vector<Utf16String> keys;
    Vector<Utf16String> values;
    u64 usage { 0 };
    bool detached { false };
    u64 next_id { 1 };

    virtual Web::StorageAPI::StorageAreaConnection connect_to_storage_area(Web::PageId, StorageEndpointType, Web::HTML::EnvironmentId const&) override
    {
        sent.append({ Sent::Kind::Connect, {}, {}, {}, {} });
        if (detached)
            return {};
        return { StorageAreaId { next_id++ }, keys, values, usage };
    }
    virtual void disconnect_from_storage_area(StorageAreaId area_id) override
    {
        sent.append({ Sent::Kind::Disconnect, area_id, {}, {}, {} });
    }
    virtual void set_storage_item(StorageAreaId area_id, Utf16String const& key, Utf16String const& value, String const& url) override
    {
        sent.append({ Sent::Kind::Set, area_id, key, value, url });
    }
    virtual void remove_storage_item(StorageAreaId area_id, Utf16String const& key, String const& url) override
    {
        sent.append({ Sent::Kind::Remove, area_id, key, {}, url });
    }
    virtual void clear_storage_area(StorageAreaId area_id, String const& url) override
    {
        sent.append({ Sent::Kind::Clear, area_id, {}, {}, url });
    }
};

struct Reported {
    Optional<Utf16String> key;
    Optional<Utf16String> old_value;
    Optional<Utf16String> new_value;
    String url;
};

struct Fixture {
    FakeClient client;
    StorageAreaMirror::Requester requester { Web::PageId { 1 }, Web::HTML::EnvironmentId::generate() };
    String const url = "https://a.example/page.html"_string;

    Fixture() { Web::StorageAPI::set_storage_area_mirror_client(&client); }
    ~Fixture() { Web::StorageAPI::set_storage_area_mirror_client(nullptr); }

    static Web::StorageAPI::StorageKey key_for(StringView origin)
    {
        return { URL::Parser::basic_parse(origin)->origin() };
    }

    NonnullRefPtr<StorageAreaMirror> mirror(StringView origin = "https://a.example"sv)
    {
        return StorageAreaMirror::obtain(StorageEndpointType::LocalStorage, {}, key_for(origin), Web::StorageAPI::StorageEndpoint::LOCAL_STORAGE_QUOTA);
    }

    static Vector<Reported>& record(StorageAreaMirror& mirror, Vector<Reported>& reports)
    {
        mirror.on_remote_change = [&reports](auto const& key, auto const& old_value, auto const& new_value, auto const& url) {
            reports.append({ key, old_value, new_value, url });
        };
        return reports;
    }
};

size_t count(Vector<Sent> const& sent, Sent::Kind kind)
{
    size_t n = 0;
    for (auto const& entry : sent)
        n += entry.kind == kind;
    return n;
}

}

TEST_CASE(prime_once_per_area_and_share_between_bottles)
{
    Fixture f;
    f.client.keys = { "a"_utf16 };
    f.client.values = { "1"_utf16 };
    f.client.usage = 2;

    auto first = f.mirror("https://shared.example"sv);
    auto second = f.mirror("https://shared.example"sv);
    EXPECT_EQ(first.ptr(), second.ptr());

    auto other_page = StorageAreaMirror::obtain(StorageEndpointType::SessionStorage, Web::PageId { 7 }, Fixture::key_for("https://shared.example"sv), {});
    EXPECT(other_page.ptr() != first.ptr());

    EXPECT_EQ(count(f.client.sent, Sent::Kind::Connect), 0u);
    EXPECT_EQ(first->get(f.requester, "a"_utf16), Optional<Utf16String> { "1"_utf16 });
    EXPECT_EQ(second->size(f.requester), 1u);
    EXPECT_EQ(second->keys(f.requester), Vector { "a"_utf16 });
    EXPECT_EQ(count(f.client.sent, Sent::Kind::Connect), 1u);
    EXPECT_EQ(first->quota_used(), 2u);
}

TEST_CASE(local_write_applies_sends_and_tracks_pending_until_acked)
{
    Fixture f;
    auto mirror = f.mirror("https://write.example"sv);

    auto result = mirror->set(f.requester, "k"_utf16, "v"_utf16, f.url);
    EXPECT(result.has<Optional<Utf16String>>());
    EXPECT_EQ(result.get<Optional<Utf16String>>(), Optional<Utf16String> {});
    EXPECT_EQ(mirror->get(f.requester, "k"_utf16), Optional<Utf16String> { "v"_utf16 });
    EXPECT_EQ(count(f.client.sent, Sent::Kind::Set), 1u);
    EXPECT_EQ(f.client.sent.last().key, "k"_utf16);
    EXPECT_EQ(f.client.sent.last().value, "v"_utf16);
    EXPECT_EQ(f.client.sent.last().url, f.url);
    EXPECT_EQ(mirror->pending_write_count(), 1u);

    auto id = *f.client.sent.last().area_id;
    StorageAreaMirror::did_acknowledge_write(id);
    EXPECT_EQ(mirror->pending_write_count(), 0u);

    // Overwrite returns the old value; removal returns the removed value; clear reports whether there was anything.
    EXPECT_EQ(mirror->set(f.requester, "k"_utf16, "w"_utf16, f.url).get<Optional<Utf16String>>(), Optional<Utf16String> { "v"_utf16 });
    EXPECT_EQ(mirror->remove(f.requester, "k"_utf16, f.url), Optional<Utf16String> { "w"_utf16 });
    EXPECT_EQ(mirror->remove(f.requester, "k"_utf16, f.url), Optional<Utf16String> {});
    EXPECT_EQ(count(f.client.sent, Sent::Kind::Remove), 1u);
    EXPECT(!mirror->clear(f.requester, f.url));
    EXPECT_EQ(count(f.client.sent, Sent::Kind::Clear), 0u);
    mirror->set(f.requester, "k"_utf16, "v"_utf16, f.url);
    EXPECT(mirror->clear(f.requester, f.url));
    EXPECT_EQ(count(f.client.sent, Sent::Kind::Clear), 1u);
    EXPECT_EQ(mirror->size(f.requester), 0u);
}

TEST_CASE(remote_changes_apply_except_over_pending_local_writes)
{
    Fixture f;
    f.client.keys = { "a"_utf16, "b"_utf16 };
    f.client.values = { "1"_utf16, "2"_utf16 };
    f.client.usage = 4;
    auto mirror = f.mirror("https://remote.example"sv);
    Vector<Reported> reports;
    Fixture::record(*mirror, reports);

    mirror->set(f.requester, "a"_utf16, "mine"_utf16, f.url);
    auto id = *f.client.sent.last().area_id;

    // The owner serialized this change before our pending write, so our mirror already reflects the later state.
    StorageAreaMirror::did_change(id, "a"_utf16, "1"_utf16, "theirs"_utf16, "https://remote.example/other.html"_string);
    EXPECT_EQ(mirror->get(f.requester, "a"_utf16), Optional<Utf16String> { "mine"_utf16 });
    EXPECT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports[0].key, Optional<Utf16String> { "a"_utf16 });
    EXPECT_EQ(reports[0].new_value, Optional<Utf16String> { "theirs"_utf16 });
    EXPECT_EQ(reports[0].url, "https://remote.example/other.html"_string);

    // A change to another key applies.
    StorageAreaMirror::did_change(id, "b"_utf16, "2"_utf16, "3"_utf16, f.url);
    EXPECT_EQ(mirror->get(f.requester, "b"_utf16), Optional<Utf16String> { "3"_utf16 });
    StorageAreaMirror::did_change(id, "b"_utf16, "3"_utf16, {}, f.url);
    EXPECT_EQ(mirror->get(f.requester, "b"_utf16), Optional<Utf16String> {});
    EXPECT_EQ(reports.size(), 3u);

    StorageAreaMirror::did_acknowledge_write(id);
    StorageAreaMirror::did_change(id, "a"_utf16, "mine"_utf16, "later"_utf16, f.url);
    EXPECT_EQ(mirror->get(f.requester, "a"_utf16), Optional<Utf16String> { "later"_utf16 });

    // While a local clear is unacknowledged, every incoming change predates it.
    mirror->clear(f.requester, f.url);
    StorageAreaMirror::did_change(id, "c"_utf16, {}, "4"_utf16, f.url);
    StorageAreaMirror::did_change(id, {}, {}, {}, f.url);
    EXPECT_EQ(mirror->size(f.requester), 0u);
    EXPECT_EQ(reports.size(), 6u);
    StorageAreaMirror::did_acknowledge_write(id);
    StorageAreaMirror::did_change(id, "c"_utf16, {}, "4"_utf16, f.url);
    EXPECT_EQ(mirror->get(f.requester, "c"_utf16), Optional<Utf16String> { "4"_utf16 });
    EXPECT_EQ(mirror->quota_used(), 2u);
}

TEST_CASE(refusal_resets_to_the_snapshot_reapplies_pending_writes_and_reports_the_diff)
{
    Fixture f;
    auto mirror = f.mirror("https://refused.example"sv);
    Vector<Reported> reports;
    Fixture::record(*mirror, reports);

    mirror->set(f.requester, "big"_utf16, "xxxx"_utf16, f.url);
    auto id = *f.client.sent.last().area_id;
    mirror->set(f.requester, "b"_utf16, "2"_utf16, f.url);
    EXPECT_EQ(mirror->pending_write_count(), 2u);

    // The owner refused the first write; the snapshot shows what another process stored in the meantime.
    StorageAreaMirror::did_refuse_write(id, { "a"_utf16 }, { "1"_utf16 });

    EXPECT_EQ(mirror->get(f.requester, "big"_utf16), Optional<Utf16String> {});
    EXPECT_EQ(mirror->get(f.requester, "a"_utf16), Optional<Utf16String> { "1"_utf16 });
    EXPECT_EQ(mirror->get(f.requester, "b"_utf16), Optional<Utf16String> { "2"_utf16 });
    EXPECT_EQ(mirror->pending_write_count(), 1u);
    EXPECT_EQ(mirror->quota_used(), 4u);

    // Two keys changed from the mirror's point of view: the refused one vanished, and "a" appeared.
    EXPECT_EQ(reports.size(), 2u);
    bool saw_big = false;
    bool saw_a = false;
    for (auto const& report : reports) {
        if (report.key == Optional<Utf16String> { "big"_utf16 }) {
            saw_big = true;
            EXPECT_EQ(report.old_value, Optional<Utf16String> { "xxxx"_utf16 });
            EXPECT_EQ(report.new_value, Optional<Utf16String> {});
        }
        if (report.key == Optional<Utf16String> { "a"_utf16 }) {
            saw_a = true;
            EXPECT_EQ(report.old_value, Optional<Utf16String> {});
            EXPECT_EQ(report.new_value, Optional<Utf16String> { "1"_utf16 });
        }
    }
    EXPECT(saw_big);
    EXPECT(saw_a);

    // A reset from the browser takes the same path, with nothing pending to re-apply afterwards.
    StorageAreaMirror::did_acknowledge_write(id);
    StorageAreaMirror::did_reset(id, {}, {});
    EXPECT_EQ(mirror->size(f.requester), 0u);
    EXPECT_EQ(mirror->quota_used(), 0u);
}

// A script can queue thousands of writes in one task; their acknowledgments arrive after it yields. Each one has to be
// cheap, and a refusal in the middle of the stream replays only the writes the owner hasn't answered yet.
TEST_CASE(a_long_queue_of_pending_writes_drains_in_order_and_a_refusal_replays_only_the_outstanding_ones)
{
    Fixture f;
    auto mirror = f.mirror("https://queue.example"sv);
    Vector<Reported> reports;
    Fixture::record(*mirror, reports);

    constexpr size_t write_count = 5000;
    for (size_t i = 0; i < write_count; ++i)
        mirror->set(f.requester, Utf16String::number(i), "v"_utf16, f.url);
    EXPECT_EQ(mirror->pending_write_count(), write_count);
    auto id = *f.client.sent.last().area_id;

    for (size_t i = 0; i < write_count - 2; ++i)
        StorageAreaMirror::did_acknowledge_write(id);
    EXPECT_EQ(mirror->pending_write_count(), 2u);

    // The owner refused the second-to-last write and describes an empty bottle: everything acknowledged is gone from
    // it, and only the last write is still in flight on top.
    StorageAreaMirror::did_refuse_write(id, {}, {});
    EXPECT_EQ(mirror->pending_write_count(), 1u);
    EXPECT_EQ(mirror->size(f.requester), 1u);
    EXPECT_EQ(mirror->get(f.requester, Utf16String::number(write_count - 1)), Optional<Utf16String> { "v"_utf16 });
    EXPECT_EQ(mirror->get(f.requester, Utf16String::number(write_count - 2)), Optional<Utf16String> {});
    EXPECT_EQ(mirror->quota_used(), Web::StorageAPI::storage_quota_size(Utf16String::number(write_count - 1)) + Web::StorageAPI::storage_quota_size("v"_utf16));

    // The diff reports every acknowledged write as gone, and nothing for the one still pending.
    EXPECT_EQ(reports.size(), write_count - 1);

    StorageAreaMirror::did_acknowledge_write(id);
    EXPECT_EQ(mirror->pending_write_count(), 0u);
}

TEST_CASE(quota_used_matches_the_jar_rule_after_mixed_writes)
{
    Fixture f;
    auto mirror = f.mirror("https://quota.example"sv);
    auto size = [](Utf16String const& key, Utf16String const& value) {
        return Web::StorageAPI::storage_quota_size(key) + Web::StorageAPI::storage_quota_size(value);
    };

    mirror->set(f.requester, "k1"_utf16, "éé"_utf16, f.url);
    mirror->set(f.requester, "k2"_utf16, "value"_utf16, f.url);
    mirror->set(f.requester, "k1"_utf16, "x"_utf16, f.url);
    (void)mirror->remove(f.requester, "k2"_utf16, f.url);
    EXPECT_EQ(mirror->quota_used(), size("k1"_utf16, "x"_utf16));

    // A write that would exceed the quota is refused locally and leaves the mirror untouched.
    auto too_large = Utf16String::repeated('y', Web::StorageAPI::StorageEndpoint::LOCAL_STORAGE_QUOTA - 2);
    auto result = mirror->set(f.requester, "k3"_utf16, too_large, f.url);
    EXPECT(result.has<WebView::StorageOperationError>());
    EXPECT_EQ(mirror->get(f.requester, "k3"_utf16), Optional<Utf16String> {});
    EXPECT_EQ(mirror->quota_used(), size("k1"_utf16, "x"_utf16));
    EXPECT_EQ(count(f.client.sent, Sent::Kind::Set), 3u);

    // One too large to ever fit is refused before the mirror is even primed.
    FakeClient fresh;
    Web::StorageAPI::set_storage_area_mirror_client(&fresh);
    auto unprimed = f.mirror("https://unprimed.example"sv);
    auto enormous = Utf16String::repeated('z', Web::StorageAPI::StorageEndpoint::LOCAL_STORAGE_QUOTA + 1);
    EXPECT(unprimed->set(f.requester, "k"_utf16, enormous, f.url).has<WebView::StorageOperationError>());
    EXPECT(fresh.sent.is_empty());
    Web::StorageAPI::set_storage_area_mirror_client(&f.client);
}

TEST_CASE(detached_mirror_reads_empty_and_refuses_writes)
{
    Fixture f;
    f.client.detached = true;
    auto mirror = f.mirror("https://detached.example"sv);

    EXPECT_EQ(mirror->size(f.requester), 0u);
    EXPECT_EQ(mirror->get(f.requester, "k"_utf16), Optional<Utf16String> {});
    EXPECT(mirror->set(f.requester, "k"_utf16, "v"_utf16, f.url).has<WebView::StorageOperationError>());
    EXPECT_EQ(mirror->remove(f.requester, "k"_utf16, f.url), Optional<Utf16String> {});
    EXPECT(!mirror->clear(f.requester, f.url));
    EXPECT_EQ(count(f.client.sent, Sent::Kind::Connect), 1u);
    EXPECT_EQ(count(f.client.sent, Sent::Kind::Set), 0u);
}

TEST_CASE(a_failed_connect_detaches_only_the_requester_it_failed_for)
{
    Fixture f;
    f.client.detached = true;
    auto mirror = f.mirror("https://partly.example"sv);
    StorageAreaMirror::Requester closing_page { Web::PageId { 9 }, Web::HTML::EnvironmentId::generate() };

    // A page the browser can't resolve (it's closing) reads empty and can't write; it isn't asked about again.
    EXPECT_EQ(mirror->size(closing_page), 0u);
    EXPECT(mirror->set(closing_page, "k"_utf16, "v"_utf16, f.url).has<WebView::StorageOperationError>());
    EXPECT_EQ(count(f.client.sent, Sent::Kind::Connect), 1u);
    EXPECT_EQ(mirror->size(closing_page), 0u);
    EXPECT_EQ(count(f.client.sent, Sent::Kind::Connect), 1u);

    // Another document of the origin in this process, which the browser does resolve, still gets its storage.
    f.client.detached = false;
    f.client.keys = { "a"_utf16 };
    f.client.values = { "1"_utf16 };
    f.client.usage = 2;
    EXPECT_EQ(mirror->get(f.requester, "a"_utf16), Optional<Utf16String> { "1"_utf16 });
    EXPECT_EQ(count(f.client.sent, Sent::Kind::Connect), 2u);
    EXPECT(mirror->set(f.requester, "k"_utf16, "v"_utf16, f.url).has<Optional<Utf16String>>());
    // And once the mirror is connected, every requester reads it.
    EXPECT_EQ(mirror->size(closing_page), 2u);
}

TEST_CASE(the_last_bottle_disconnects_the_mirror)
{
    Fixture f;
    {
        auto mirror = f.mirror("https://lastbottle.example"sv);
        auto another = f.mirror("https://lastbottle.example"sv);
        mirror->size(f.requester);
        EXPECT_EQ(count(f.client.sent, Sent::Kind::Disconnect), 0u);
    }
    EXPECT_EQ(count(f.client.sent, Sent::Kind::Disconnect), 1u);

    // A mirror that was never primed has nothing to disconnect from.
    {
        auto never = f.mirror("https://never.example"sv);
    }
    EXPECT_EQ(count(f.client.sent, Sent::Kind::Disconnect), 1u);
}
