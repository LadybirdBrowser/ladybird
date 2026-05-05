/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibPaintServer/Compositor/DrawCommands.h>

namespace PaintServer {

enum CommandClassification : u8 {
    Draw,
    State,
    Other
};

static CommandClassification classify_command(CommandType command_type)
{
    switch (command_type) {
    case CommandType::ClearRect:
    case CommandType::DrawGlyphRun:
    case CommandType::FillRect:
    case CommandType::FillRectWithRoundedCorners:
    case CommandType::DrawScaledImage:
    case CommandType::DrawRepeatedImage:
    case CommandType::DrawLine:
    case CommandType::DrawRect:
    case CommandType::DrawExternalContent:
    case CommandType::PaintLinearGradient:
    case CommandType::PaintRadialGradient:
    case CommandType::PaintConicGradient:
    case CommandType::PaintOuterBoxShadow:
    case CommandType::PaintInnerBoxShadow:
    case CommandType::PaintTextShadow:
    case CommandType::StrokePath:
    case CommandType::FillPath:
    case CommandType::DrawEllipse:
    case CommandType::FillEllipse:
    case CommandType::PaintScrollBar:
        return CommandClassification::Draw;
    case CommandType::Save:
    case CommandType::SaveLayer:
    case CommandType::Restore:
    case CommandType::ResetCanvasState:
    case CommandType::Translate:
    case CommandType::AddClipRect:
    case CommandType::AddRoundedRectClip:
    case CommandType::AddClipPath:
    case CommandType::ApplyEffects:
    case CommandType::ApplyBackdropFilter:
    case CommandType::SetTransform:
    case CommandType::ApplyTransform:
        return CommandClassification::State;
    default:
        return CommandClassification::Other;
    }
}

bool is_draw_command(CommandType command_type)
{
    return classify_command(command_type) == CommandClassification::Draw;
}

bool is_state_command(CommandType command_type)
{
    return classify_command(command_type) == CommandClassification::State;
}

StringView command_type_name(CommandType command_type)
{
    switch (command_type) {
    case CommandType::Invalid:
        return "Invalid"sv;
    case CommandType::ClearRect:
        return "ClearRect"sv;
    case CommandType::DrawGlyphRun:
        return "DrawGlyphRun"sv;
    case CommandType::FillRect:
        return "FillRect"sv;
    case CommandType::Save:
        return "Save"sv;
    case CommandType::SaveLayer:
        return "SaveLayer"sv;
    case CommandType::Restore:
        return "Restore"sv;
    case CommandType::ResetCanvasState:
        return "ResetCanvasState"sv;
    case CommandType::Translate:
        return "Translate"sv;
    case CommandType::AddClipRect:
        return "AddClipRect"sv;
    case CommandType::FillRectWithRoundedCorners:
        return "FillRectWithRoundedCorners"sv;
    case CommandType::AddRoundedRectClip:
        return "AddRoundedRectClip"sv;
    case CommandType::DrawScaledImage:
        return "DrawScaledImage"sv;
    case CommandType::DrawRepeatedImage:
        return "DrawRepeatedImage"sv;
    case CommandType::DrawLine:
        return "DrawLine"sv;
    case CommandType::DrawRect:
        return "DrawRect"sv;
    case CommandType::StrokePath:
        return "StrokePath"sv;
    case CommandType::FillPath:
        return "FillPath"sv;
    case CommandType::DrawEllipse:
        return "DrawEllipse"sv;
    case CommandType::FillEllipse:
        return "FillEllipse"sv;
    case CommandType::PaintTextShadow:
        return "PaintTextShadow"sv;
    case CommandType::PaintLinearGradient:
        return "PaintLinearGradient"sv;
    case CommandType::PaintRadialGradient:
        return "PaintRadialGradient"sv;
    case CommandType::PaintConicGradient:
        return "PaintConicGradient"sv;
    case CommandType::PaintOuterBoxShadow:
        return "PaintOuterBoxShadow"sv;
    case CommandType::PaintInnerBoxShadow:
        return "PaintInnerBoxShadow"sv;
    case CommandType::ApplyEffects:
        return "ApplyEffects"sv;
    case CommandType::ApplyBackdropFilter:
        return "ApplyBackdropFilter"sv;
    case CommandType::SetTransform:
        return "SetTransform"sv;
    case CommandType::DrawExternalContent:
        return "DrawExternalContent"sv;
    case CommandType::PaintScrollBar:
        return "PaintScrollBar"sv;
    case CommandType::AddClipPath:
        return "AddClipPath"sv;
    case CommandType::ApplyTransform:
        return "ApplyTransform"sv;
    case CommandType::Count:
        return "Count"sv;
    }

    return "Unknown"sv;
}

}
