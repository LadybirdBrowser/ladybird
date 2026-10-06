/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Runtime/PromiseJob.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

using namespace EmbeddingABI;

ThrowCompletionOr<Value> PromiseJob::run() const
{
    return completion_from_abi<Value>(js_promise_job_run(vm_to_abi(VM::the()), reinterpret_cast<JSPromiseJob*>(m_job.ptr())));
}

}
