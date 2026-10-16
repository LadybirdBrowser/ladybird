/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/NeverDestroyed.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/Function.h>
#include <LibGC/Heap.h>
#include <LibGC/Root.h>
#include <LibGC/WeakInlines.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWeb/Page/ResourceCache.h>

class ResourceCacheHolder final : public GC::Cell {
    GC_CELL(ResourceCacheHolder, GC::Cell);
    GC_DECLARE_ALLOCATOR(ResourceCacheHolder);

public:
    Web::ResourceCache<int> cache { 32, 1024, [](int const&) -> size_t { return 0; } };

private:
    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        cache.visit_edges(visitor);
    }
};

GC_DEFINE_ALLOCATOR(ResourceCacheHolder);

static GC::Heap& test_heap()
{
    static AK::NeverDestroyed<GC::Heap> heap([](auto&) { });
    return *heap;
}

TEST_SETUP
{
    GC::Heap::set_default_heap_for_testing(test_heap());
}

static URL::URL parse_url(StringView url)
{
    return URL::Parser::basic_parse(url).release_value();
}

TEST_CASE(entry_that_grows_past_the_memory_limit_is_evicted)
{
    HashMap<int, size_t> value_sizes;
    Web::ResourceCache<int> cache { 32, 1024, [&](int const& value) { return value_sizes.get(value).value(); } };
    auto growing_url = parse_url("data:,growing"sv);
    auto other_url = parse_url("data:,other"sv);

    value_sizes.set(1, 100);
    value_sizes.set(2, 100);
    cache.set(growing_url, 1);
    cache.set(other_url, 2);

    value_sizes.set(1, 2048);
    EXPECT(cache.get(other_url).has_value());
    EXPECT(!cache.get(growing_url).has_value());
}

TEST_CASE(entries_that_grow_past_the_memory_limit_evict_the_least_recently_used)
{
    HashMap<int, size_t> value_sizes;
    Web::ResourceCache<int> cache { 32, 1024, [&](int const& value) { return value_sizes.get(value).value(); } };
    auto older_url = parse_url("data:,older"sv);
    auto newer_url = parse_url("data:,newer"sv);

    value_sizes.set(1, 100);
    value_sizes.set(2, 100);
    cache.set(older_url, 1);
    cache.set(newer_url, 2);

    value_sizes.set(1, 600);
    value_sizes.set(2, 600);
    EXPECT(cache.get(newer_url).has_value());
    EXPECT(!cache.get(older_url).has_value());
}

static constexpr size_t PENDING_LOAD_CALLBACK_COUNT = 16;

struct PendingLoadCallbackState {
    Vector<GC::Weak<Web::ResourceCache<int>::PendingLoadCallback>> callbacks;
    size_t invocation_count { 0 };
    size_t callbacks_alive_after_collection { 0 };
};

static NEVER_INLINE void scrub_stack()
{
    u8 volatile filler[16 * KiB];
    for (size_t i = 0; i < sizeof(filler); ++i)
        filler[i] = 0;
}

static NEVER_INLINE void wait_for_pending_load_with_callbacks_that_collect_garbage(Web::ResourceCache<int>& cache, URL::URL const& url, PendingLoadCallbackState& state)
{
    cache.begin_pending_load(url);
    for (size_t i = 0; i < PENDING_LOAD_CALLBACK_COUNT; ++i) {
        auto callback = GC::create_function(test_heap(), [&state](Optional<int>) {
            if (state.invocation_count++ != 0)
                return;
            test_heap().collect_garbage();
            for (auto const& callback : state.callbacks) {
                if (callback.ptr())
                    ++state.callbacks_alive_after_collection;
            }
        });
        state.callbacks.append(*callback);
        cache.wait_for_pending_load(url, callback);
    }
}

TEST_CASE(pending_load_callbacks_survive_a_collection_while_a_load_finishes)
{
    auto holder = GC::Root { test_heap().allocate<ResourceCacheHolder>() };
    auto url = parse_url("data:,pending"sv);
    PendingLoadCallbackState state;
    wait_for_pending_load_with_callbacks_that_collect_garbage(holder->cache, url, state);
    scrub_stack();

    holder->cache.set(url, 1);
    EXPECT_EQ(state.callbacks_alive_after_collection, PENDING_LOAD_CALLBACK_COUNT);
    EXPECT_EQ(state.invocation_count, PENDING_LOAD_CALLBACK_COUNT);
}

TEST_CASE(pending_load_callbacks_survive_a_collection_while_a_load_fails)
{
    auto holder = GC::Root { test_heap().allocate<ResourceCacheHolder>() };
    auto url = parse_url("data:,pending"sv);
    PendingLoadCallbackState state;
    wait_for_pending_load_with_callbacks_that_collect_garbage(holder->cache, url, state);
    scrub_stack();

    holder->cache.finish_pending_load_without_value(url);
    EXPECT_EQ(state.callbacks_alive_after_collection, PENDING_LOAD_CALLBACK_COUNT);
    EXPECT_EQ(state.invocation_count, PENDING_LOAD_CALLBACK_COUNT);
}
