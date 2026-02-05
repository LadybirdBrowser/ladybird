/*
 * Copyright (c) 2025, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#define DEFINE_CACHED_ATTRIBUTE(name)                                                              \
    GC::Ptr<JS::Object> cached_##name() const { return m_cached_##name; }                          \
    void set_cached_##name(GC::Ptr<JS::Object> cached_##name) { m_cached_##name = cached_##name; } \
                                                                                                   \
private:                                                                                           \
    GC::Ptr<JS::Object> m_cached_##name;                                                           \
                                                                                                   \
public:

#define VISIT_CACHED_ATTRIBUTE(name) visitor.visit(m_cached_##name)
