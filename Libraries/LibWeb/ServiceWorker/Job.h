/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibURL/URL.h>
#include <LibWeb/Bindings/ServiceWorkerRegistrationPrototype.h>
#include <LibWeb/Bindings/WorkerPrototype.h>
#include <LibWeb/StorageAPI/StorageKey.h>

namespace Web::ServiceWorker {

struct Job;
using JobQueue = GC::MarkedVector<GC::Ref<Job>>;

// https://w3c.github.io/ServiceWorker/#dfn-job
// FIXME: Consider not making this GC allocated, and give a special JobQueue class responsibility for its referenced GC objects
struct Job : public JS::Cell {
    GC_CELL(Job, JS::Cell)
    GC_DECLARE_ALLOCATOR(Job);

public:
    enum class Type : u8 {
        Register,
        Update,
        Unregister,
    };

    // https://w3c.github.io/ServiceWorker/#create-job
    static GC::Ref<Job> create(JS::VM&, Type, StorageAPI::StorageKey, URL::URL scope_url, URL::URL script_url, GC::Ptr<WebIDL::Promise>, GC::Ptr<HTML::EnvironmentSettingsObject> client);

    virtual ~Job() override;

    Type job_type;                      // https://w3c.github.io/ServiceWorker/#dfn-job-type
    StorageAPI::StorageKey storage_key; // https://w3c.github.io/ServiceWorker/#job-storage-key
    URL::URL scope_url;
    URL::URL script_url;
    Bindings::WorkerType worker_type = Bindings::WorkerType::Classic;
    // FIXME: The spec sometimes omits setting update_via_cache after CreateJob. Default to the default value for ServiceWorkerRegistrations
    Bindings::ServiceWorkerUpdateViaCache update_via_cache = Bindings::ServiceWorkerUpdateViaCache::Imports;
    GC::Ptr<HTML::EnvironmentSettingsObject> client = nullptr;
    Optional<URL::URL> referrer;
    // FIXME: Spec just references this as an ECMAScript promise https://github.com/w3c/ServiceWorker/issues/1731
    GC::Ptr<WebIDL::Promise> job_promise = nullptr;
    RawPtr<JobQueue> containing_job_queue = nullptr;
    Vector<GC::Ref<Job>> list_of_equivalent_jobs;
    bool force_cache_bypass = false;

    // https://w3c.github.io/ServiceWorker/#dfn-job-equivalent
    friend bool operator==(Job const& a, Job const& b)
    {
        if (a.job_type != b.job_type)
            return false;
        switch (a.job_type) {
        case Type::Register:
        case Type::Update:
            return a.scope_url == b.scope_url
                && a.script_url == b.script_url
                && a.worker_type == b.worker_type
                && a.update_via_cache == b.update_via_cache;
        case Type::Unregister:
            return a.scope_url == b.scope_url;
        }
    }

private:
    virtual void visit_edges(JS::Cell::Visitor& visitor) override;

    Job(Type, StorageAPI::StorageKey, URL::URL scope_url, URL::URL script_url, GC::Ptr<WebIDL::Promise>, GC::Ptr<HTML::EnvironmentSettingsObject> client);
};

// https://w3c.github.io/ServiceWorker/#schedule-job-algorithm
void schedule_job(JS::VM&, GC::Ref<Job>);

}
