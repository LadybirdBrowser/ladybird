/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/GPUSupportedLimitsPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>

namespace Web::WebGPU {

// https://www.w3.org/TR/webgpu/#supported-limits
class GPUSupportedLimits final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(GPUSupportedLimits, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(GPUSupportedLimits);

    static JS::ThrowCompletionOr<GC::Ref<GPUSupportedLimits>> create(JS::Realm&);

    size_t max_texture_dimension1d() const { return m_max_texture_dimension1d; }
    void set_max_texture_dimension1d(size_t const value) { m_max_texture_dimension1d = value; }

    size_t max_texture_dimension2d() const { return m_max_texture_dimension2d; }
    void set_max_texture_dimension2d(size_t const value) { m_max_texture_dimension2d = value; }

    size_t max_texture_dimension3d() const { return m_max_texture_dimension3d; }
    void set_max_texture_dimension3d(size_t const value) { m_max_texture_dimension3d = value; }

    size_t max_texture_array_layers() const { return m_max_texture_array_layers; }
    void set_max_texture_array_layers(size_t const value) { m_max_texture_array_layers = value; }

    size_t max_bind_groups() const { return m_max_bind_groups; }
    void set_max_bind_groups(size_t const value) { m_max_bind_groups = value; }

    size_t max_bind_groups_plus_vertex_buffers() const { return m_max_bind_groups_plus_vertex_buffers; }
    void set_max_bind_groups_plus_vertex_buffers(size_t const value) { m_max_bind_groups_plus_vertex_buffers = value; }

    size_t max_bindings_per_bind_group() const { return m_max_bindings_per_bind_group; }
    void set_max_bindings_per_bind_group(size_t const value) { m_max_bindings_per_bind_group = value; }

    size_t max_dynamic_uniform_buffers_per_pipeline_layout() const { return m_max_dynamic_uniform_buffers_per_pipeline_layout; }
    void set_max_dynamic_uniform_buffers_per_pipeline_layout(size_t const value) { m_max_dynamic_uniform_buffers_per_pipeline_layout = value; }

    size_t max_dynamic_storage_buffers_per_pipeline_layout() const { return m_max_dynamic_storage_buffers_per_pipeline_layout; }
    void set_max_dynamic_storage_buffers_per_pipeline_layout(size_t const value) { m_max_dynamic_storage_buffers_per_pipeline_layout = value; }

    size_t max_sampled_textures_per_shader_stage() const { return m_max_sampled_textures_per_shader_stage; }
    void set_max_sampled_textures_per_shader_stage(size_t const value) { m_max_sampled_textures_per_shader_stage = value; }

    size_t max_samplers_per_shader_stage() const { return m_max_samplers_per_shader_stage; }
    void set_max_samplers_per_shader_stage(size_t const value) { m_max_samplers_per_shader_stage = value; }

    size_t max_storage_buffers_per_shader_stage() const { return m_max_storage_buffers_per_shader_stage; }
    void set_max_storage_buffers_per_shader_stage(size_t const value) { m_max_storage_buffers_per_shader_stage = value; }

    size_t max_storage_textures_per_shader_stage() const { return m_max_storage_textures_per_shader_stage; }
    void set_max_storage_textures_per_shader_stage(size_t const value) { m_max_storage_textures_per_shader_stage = value; }

    size_t max_uniform_buffers_per_shader_stage() const { return m_max_uniform_buffers_per_shader_stage; }
    void set_max_uniform_buffers_per_shader_stage(size_t const value) { m_max_uniform_buffers_per_shader_stage = value; }

    size_t max_uniform_buffer_binding_size() const { return m_max_uniform_buffer_binding_size; }
    void set_max_uniform_buffer_binding_size(size_t const value) { m_max_uniform_buffer_binding_size = value; }

    size_t max_storage_buffer_binding_size() const { return m_max_storage_buffer_binding_size; }
    void set_max_storage_buffer_binding_size(size_t const value) { m_max_storage_buffer_binding_size = value; }

    size_t min_uniform_buffer_offset_alignment() const { return m_min_uniform_buffer_offset_alignment; }
    void set_min_uniform_buffer_offset_alignment(size_t const value) { m_min_uniform_buffer_offset_alignment = value; }

    size_t min_storage_buffer_offset_alignment() const { return m_min_storage_buffer_offset_alignment; }
    void set_min_storage_buffer_offset_alignment(size_t const value) { m_min_storage_buffer_offset_alignment = value; }

    size_t max_vertex_buffers() const { return m_max_vertex_buffers; }
    void set_max_vertex_buffers(size_t const value) { m_max_vertex_buffers = value; }

    size_t max_buffer_size() const { return m_max_buffer_size; }
    void set_max_buffer_size(size_t const value) { m_max_buffer_size = value; }

    size_t max_vertex_attributes() const { return m_max_vertex_attributes; }
    void set_max_vertex_attributes(size_t const value) { m_max_vertex_attributes = value; }

    size_t max_vertex_buffer_array_stride() const { return m_max_vertex_buffer_array_stride; }
    void set_max_vertex_buffer_array_stride(size_t const value) { m_max_vertex_buffer_array_stride = value; }

    size_t max_inter_stage_shader_variables() const { return m_max_inter_stage_shader_variables; }
    void set_max_inter_stage_shader_variables(size_t const value) { m_max_inter_stage_shader_variables = value; }

    size_t max_color_attachments() const { return m_max_color_attachments; }
    void set_max_color_attachments(size_t const value) { m_max_color_attachments = value; }

    size_t max_color_attachment_bytes_per_sample() const { return m_max_color_attachment_bytes_per_sample; }
    void set_max_color_attachment_bytes_per_sample(size_t const value) { m_max_color_attachment_bytes_per_sample = value; }

    size_t max_compute_workgroup_storage_size() const { return m_max_compute_workgroup_storage_size; }
    void set_max_compute_workgroup_storage_size(size_t const value) { m_max_compute_workgroup_storage_size = value; }

    size_t max_compute_invocations_per_workgroup() const { return m_max_compute_invocations_per_workgroup; }
    void set_max_compute_invocations_per_workgroup(size_t const value) { m_max_compute_invocations_per_workgroup = value; }

    size_t max_compute_workgroup_size_x() const { return m_max_compute_workgroup_size_x; }
    void set_max_compute_workgroup_size_x(size_t const value) { m_max_compute_workgroup_size_x = value; }

    size_t max_compute_workgroup_size_y() const { return m_max_compute_workgroup_size_y; }
    void set_max_compute_workgroup_size_y(size_t const value) { m_max_compute_workgroup_size_y = value; }

    size_t max_compute_workgroup_size_z() const { return m_max_compute_workgroup_size_z; }
    void set_max_compute_workgroup_size_z(size_t const value) { m_max_compute_workgroup_size_z = value; }

    size_t max_compute_workgroups_per_dimension() const { return m_max_compute_workgroups_per_dimension; }
    void set_max_compute_workgroups_per_dimension(size_t const value) { m_max_compute_workgroups_per_dimension = value; }

private:
    explicit GPUSupportedLimits(JS::Realm&);

    void initialize(JS::Realm&) override;

    void visit_edges(Visitor&) override;

    size_t m_max_texture_dimension1d { 0 };
    size_t m_max_texture_dimension2d { 0 };
    size_t m_max_texture_dimension3d { 0 };
    size_t m_max_texture_array_layers { 0 };
    size_t m_max_bind_groups { 0 };
    size_t m_max_bind_groups_plus_vertex_buffers { 0 };
    size_t m_max_bindings_per_bind_group { 0 };
    size_t m_max_dynamic_uniform_buffers_per_pipeline_layout { 0 };
    size_t m_max_dynamic_storage_buffers_per_pipeline_layout { 0 };
    size_t m_max_sampled_textures_per_shader_stage { 0 };
    size_t m_max_samplers_per_shader_stage { 0 };
    size_t m_max_storage_buffers_per_shader_stage { 0 };
    size_t m_max_storage_textures_per_shader_stage { 0 };
    size_t m_max_uniform_buffers_per_shader_stage { 0 };
    size_t m_max_uniform_buffer_binding_size { 0 };
    size_t m_max_storage_buffer_binding_size { 0 };
    size_t m_min_uniform_buffer_offset_alignment { 0 };
    size_t m_min_storage_buffer_offset_alignment { 0 };
    size_t m_max_vertex_buffers { 0 };
    size_t m_max_buffer_size { 0 };
    size_t m_max_vertex_attributes { 0 };
    size_t m_max_vertex_buffer_array_stride { 0 };
    size_t m_max_inter_stage_shader_variables { 0 };
    size_t m_max_color_attachments { 0 };
    size_t m_max_color_attachment_bytes_per_sample { 0 };
    size_t m_max_compute_workgroup_storage_size { 0 };
    size_t m_max_compute_invocations_per_workgroup { 0 };
    size_t m_max_compute_workgroup_size_x { 0 };
    size_t m_max_compute_workgroup_size_y { 0 };
    size_t m_max_compute_workgroup_size_z { 0 };
    size_t m_max_compute_workgroups_per_dimension { 0 };
};

};
