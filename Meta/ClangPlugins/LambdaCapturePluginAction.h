/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <clang/Tooling/Tooling.h>

class LambdaCapturePluginAction : public clang::PluginASTAction {
public:
    virtual bool ParseArgs(clang::CompilerInstance const&, std::vector<std::string> const&) override
    {
        return true;
    }

    virtual std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance&, llvm::StringRef) override;

    ActionType getActionType() override
    {
        return AddAfterMainAction;
    }
};
