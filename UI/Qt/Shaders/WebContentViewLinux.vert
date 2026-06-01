/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#version 450

layout(push_constant) uniform PushConstants {
    vec2 target_size;
    vec2 content_size;
    vec2 source_size;
} push_constants;

layout(location = 0) out vec2 texture_coordinates;

void main()
{
    vec2 unit_positions[4] = vec2[](
        vec2(0.0, 0.0),
        vec2(1.0, 0.0),
        vec2(0.0, 1.0),
        vec2(1.0, 1.0));

    vec2 unit_position = unit_positions[gl_VertexIndex];
    vec2 target_position = unit_position * push_constants.content_size;
    vec2 clip_position = target_position / push_constants.target_size * 2.0 - 1.0;

    gl_Position = vec4(clip_position, 0.0, 1.0);
    texture_coordinates = target_position / push_constants.source_size;
}
