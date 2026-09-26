/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Span.h>
#include <AK/Vector.h>
#include <linux/filter.h>

namespace Sandbox {

class SeccompPolicy {
public:
    SeccompPolicy();

    void deny_readonly_filesystem_probes();
    void deny_current_directory_queries();
    void allow_readonly_file_opens();
    void allow_filesystem_metadata_queries();
    void allow_filesystem_writes();
    void allow_file_descriptor_operations();
    void allow_process_creation();
    void allow_ipc();
    void broker_unix_socket_connections();
    void allow_pulseaudio_client_file_operations();
    void allow_network();
    void allow_memory_without_executable_mappings();
    void allow_executable_memory_mappings();
    void allow_writable_executable_memory_mappings();
    void allow_threads();
    void allow_signals();
    void allow_clocks();
    void allow_gpu_device_operations();
    void allow_process_metadata();
    void allow_common_runtime();
    void allow_prctl();
    void allow_exit();

    [[nodiscard]] ErrorOr<void> install();

private:
    void append(sock_filter);
    void append_allow_socket_with_domains(ReadonlySpan<u32> domains);
    void append_allow_socket_options(u32 syscall_number, u32 level, ReadonlySpan<u32> options);
    void append_architecture_check();
    void append_load_syscall_number();
    void append_kill();

    Vector<sock_filter> m_filter;
};

}
