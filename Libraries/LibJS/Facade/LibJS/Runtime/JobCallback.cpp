/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Runtime/JobCallback.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

using namespace EmbeddingABI;

GC::Ref<JobCallback> JobCallback::create(VM& vm, FunctionObject& callback, GC::Ptr<GC::Cell> custom_data)
{
    return cell_ref_from_abi<JobCallback>(js_realm_job_callback_create(vm_to_abi(vm), object_to_abi(callback), custom_data.ptr()));
}

FunctionObject& JobCallback::callback()
{
    return cell_ref_from_abi<FunctionObject>(js_realm_job_callback_callback(cell_to_abi<JSJobCallback>(*this)));
}

GC::Ptr<GC::Cell> JobCallback::custom_data() const
{
    return static_cast<GC::Cell*>(js_realm_job_callback_custom_data(cell_to_abi<JSJobCallback>(*this)));
}

}
