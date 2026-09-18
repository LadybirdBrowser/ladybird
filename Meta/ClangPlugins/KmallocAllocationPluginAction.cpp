/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "KmallocAllocationPluginAction.h"
#include <clang/AST/DeclTemplate.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Frontend/FrontendPluginRegistry.h>

static constexpr char const* kmalloc_tag_name = "AllocatedWithKmallocTag";
static constexpr char const* system_allocator_opt_out_name = "AllocatedWithSystemAllocator";
static constexpr char const* custom_allocator_opt_out_name = "AllocatedWithCustomAllocator";

static bool is_inside_namespace_ak(clang::DeclContext const* context)
{
    for (; context; context = context->getParent()) {
        if (auto const* namespace_decl = llvm::dyn_cast<clang::NamespaceDecl>(context)) {
            if (namespace_decl->getName() == "AK" && namespace_decl->getParent()->isTranslationUnit())
                return true;
        }
    }
    return false;
}

static std::string primary_template_name(clang::CXXRecordDecl const& record)
{
    if (auto const* specialization = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(&record))
        return specialization->getSpecializedTemplate()->getQualifiedNameAsString();
    return record.getQualifiedNameAsString();
}

static bool is_adopting_pointer_template(llvm::StringRef name)
{
    return name == "AK::NonnullOwnPtr" || name == "AK::NonnullRefPtr" || name == "AK::RefPtr";
}

static bool is_allocating_new_expression(clang::CXXNewExpr const& new_expression)
{
    if (new_expression.getNumPlacementArgs() == 0)
        return true;
    if (new_expression.getNumPlacementArgs() != 1)
        return false;
    auto const* tag = new_expression.getPlacementArg(0)->getType().getNonReferenceType()->getAsCXXRecordDecl();
    return tag && tag->getQualifiedNameAsString() == "std::nothrow_t";
}

static bool first_parameter_is_adopt_tag(clang::FunctionDecl const& function)
{
    if (function.getNumParams() == 0)
        return false;
    auto const* tag = function.getParamDecl(0)->getType()->getAsTagDecl();
    return tag && tag->getName() == "AdoptTag";
}

bool KmallocAllocationVisitor::is_project_record(clang::CXXRecordDecl const& record) const
{
    auto const* definition = record.getDefinition();
    if (!definition || definition->isDependentType())
        return false;
    auto location = definition->getLocation();
    if (location.isInvalid())
        return false;
    return !m_context.getSourceManager().isInSystemHeader(location);
}

bool KmallocAllocationVisitor::record_declares_kmalloc_tag(clang::CXXRecordDecl const& record) const
{
    auto const* definition = record.getDefinition();
    if (!definition)
        return false;
    auto name = clang::DeclarationName(&m_context.Idents.get(kmalloc_tag_name));
    for (auto const* declaration : definition->lookup(name)) {
        if (llvm::isa<clang::TypedefNameDecl>(declaration))
            return true;
    }
    return false;
}

bool KmallocAllocationVisitor::record_has_kmalloc_tag(clang::CXXRecordDecl const& record) const
{
    auto const* definition = record.getDefinition();
    if (!definition)
        return false;
    if (record_declares_kmalloc_tag(*definition))
        return true;

    bool found = false;
    definition->forallBases([&](clang::CXXRecordDecl const* base) {
        if (record_declares_kmalloc_tag(*base)) {
            found = true;
            return false;
        }
        return true;
    });
    return found;
}

bool KmallocAllocationVisitor::is_opted_out(clang::QualType type) const
{
    return m_opted_out_types.contains(type.getCanonicalType().getUnqualifiedType().getTypePtr());
}

bool KmallocAllocationVisitor::use_site_is_inside_ak() const
{
    if (m_function_stack.empty())
        return false;
    return is_inside_namespace_ak(m_function_stack.back()->getDeclContext());
}

void KmallocAllocationVisitor::check_opt_out(clang::VarTemplateSpecializationDecl const& specialization)
{
    auto template_name = specialization.getSpecializedTemplate()->getName();
    bool is_system_opt_out = template_name == system_allocator_opt_out_name;
    bool is_custom_opt_out = template_name == custom_allocator_opt_out_name;
    if (!is_system_opt_out && !is_custom_opt_out)
        return;
    if (specialization.getSpecializationKind() != clang::TSK_ExplicitSpecialization)
        return;
    if (llvm::isa<clang::VarTemplatePartialSpecializationDecl>(&specialization))
        return;

    auto const& arguments = specialization.getTemplateArgs();
    if (arguments.size() != 1 || arguments[0].getKind() != clang::TemplateArgument::Type)
        return;
    auto type = arguments[0].getAsType();

    if (auto const* value = specialization.evaluateValue(); value && value->isInt() && value->getInt().isZero())
        return;

    m_opted_out_types.insert(type.getCanonicalType().getUnqualifiedType().getTypePtr());

    auto const* record = type->getAsCXXRecordDecl();
    if (!record || !record->getDefinition())
        return;

    auto& diag_engine = m_context.getDiagnostics();
    if (is_system_opt_out) {
        if (is_project_record(*record)) {
            auto diag_id = diag_engine.getCustomDiagID(clang::DiagnosticsEngine::Error, "AllocatedWithSystemAllocator names %0, which is defined in this project. Use AK_ALLOC_WITH_KMALLOC or AllocatedWithCustomAllocator instead");
            diag_engine.Report(specialization.getLocation(), diag_id) << record;
        }
        return;
    }

    if (record_has_kmalloc_tag(*record)) {
        auto diag_id = diag_engine.getCustomDiagID(clang::DiagnosticsEngine::Error, "AllocatedWithCustomAllocator names %0, which already allocates with kmalloc");
        diag_engine.Report(specialization.getLocation(), diag_id) << record;
        return;
    }
    if (!is_project_record(*record)) {
        auto diag_id = diag_engine.getCustomDiagID(clang::DiagnosticsEngine::Error, "AllocatedWithCustomAllocator names %0, which is defined outside this project. Use AllocatedWithSystemAllocator instead");
        diag_engine.Report(specialization.getLocation(), diag_id) << record;
    }
}

void KmallocAllocationVisitor::collect_allocator_opt_outs()
{
    // The opt-out templates live at global scope, so their explicit specializations do too.
    for (auto const* declaration : m_context.getTranslationUnitDecl()->decls()) {
        if (auto const* specialization = llvm::dyn_cast<clang::VarTemplateSpecializationDecl>(declaration))
            check_opt_out(*specialization);
    }
}

bool KmallocAllocationVisitor::TraverseDecl(clang::Decl* declaration)
{
    // Declarations from other libraries cannot allocate project types unless a project type is passed to one of
    // their templates, so skipping them keeps the traversal proportional to the project's own code.
    if (declaration) {
        auto location = declaration->getLocation();
        if (location.isValid() && m_context.getSourceManager().isInSystemHeader(location))
            return true;
    }

    auto* function = llvm::dyn_cast_or_null<clang::FunctionDecl>(declaration);
    if (!function)
        return RecursiveASTVisitor::TraverseDecl(declaration);

    m_function_stack.push_back(function);
    bool result = RecursiveASTVisitor::TraverseDecl(declaration);
    m_function_stack.pop_back();
    return result;
}

bool KmallocAllocationVisitor::TraverseClassTemplateDecl(clang::ClassTemplateDecl* declaration)
{
    return traverse_template(declaration, [&](auto* node) { return RecursiveASTVisitor::TraverseClassTemplateDecl(node); });
}

bool KmallocAllocationVisitor::TraverseFunctionTemplateDecl(clang::FunctionTemplateDecl* declaration)
{
    return traverse_template(declaration, [&](auto* node) { return RecursiveASTVisitor::TraverseFunctionTemplateDecl(node); });
}

bool KmallocAllocationVisitor::TraverseRequiresExpr(clang::RequiresExpr* expression)
{
    return traverse_unevaluated(expression, [&](auto* node) { return RecursiveASTVisitor::TraverseRequiresExpr(node); });
}

bool KmallocAllocationVisitor::TraverseCXXNoexceptExpr(clang::CXXNoexceptExpr* expression)
{
    return traverse_unevaluated(expression, [&](auto* node) { return RecursiveASTVisitor::TraverseCXXNoexceptExpr(node); });
}

bool KmallocAllocationVisitor::TraverseUnaryExprOrTypeTraitExpr(clang::UnaryExprOrTypeTraitExpr* expression)
{
    return traverse_unevaluated(expression, [&](auto* node) { return RecursiveASTVisitor::TraverseUnaryExprOrTypeTraitExpr(node); });
}

bool KmallocAllocationVisitor::TraverseTypeTraitExpr(clang::TypeTraitExpr* expression)
{
    return traverse_unevaluated(expression, [&](auto* node) { return RecursiveASTVisitor::TraverseTypeTraitExpr(node); });
}

bool KmallocAllocationVisitor::TraverseDecltypeTypeLoc(clang::DecltypeTypeLoc type_loc)
{
    return traverse_unevaluated(type_loc, [&](auto node) { return RecursiveASTVisitor::TraverseDecltypeTypeLoc(node); });
}

bool KmallocAllocationVisitor::VisitCXXNewExpr(clang::CXXNewExpr* new_expression)
{
    if (m_unevaluated_depth > 0)
        return true;
    if (is_allocating_new_expression(*new_expression))
        mark_template_candidate();

    auto const* operator_new = new_expression->getOperatorNew();
    if (!operator_new)
        return true;
    if (llvm::isa<clang::CXXMethodDecl>(operator_new))
        return true;
    if (!operator_new->isReplaceableGlobalAllocationFunction())
        return true;

    auto const* record = new_expression->getAllocatedType()->getAsCXXRecordDecl();
    if (!record || !is_project_record(*record))
        return true;
    if (is_opted_out(new_expression->getAllocatedType()))
        return true;

    auto& diag_engine = m_context.getDiagnostics();
    if (record_has_kmalloc_tag(*record)) {
        auto diag_id = diag_engine.getCustomDiagID(clang::DiagnosticsEngine::Error, "%0 allocates with kmalloc, but this new-expression bypasses its operator new");
        diag_engine.Report(new_expression->getBeginLoc(), diag_id) << record;
        return true;
    }

    auto diag_id = diag_engine.getCustomDiagID(clang::DiagnosticsEngine::Error, "%0 is allocated with the system allocator. Add AK_ALLOC_WITH_KMALLOC to the class, or specialize AllocatedWithSystemAllocator or AllocatedWithCustomAllocator for it");
    diag_engine.Report(new_expression->getBeginLoc(), diag_id) << record;
    auto note_id = diag_engine.getCustomDiagID(clang::DiagnosticsEngine::Note, "%0 is defined here");
    diag_engine.Report(record->getDefinition()->getLocation(), note_id) << record;
    return true;
}

bool KmallocAllocationVisitor::VisitCXXConstructExpr(clang::CXXConstructExpr* construct_expression)
{
    if (m_unevaluated_depth > 0)
        return true;

    auto const* constructor = construct_expression->getConstructor();
    if (!constructor || !first_parameter_is_adopt_tag(*constructor))
        return true;

    if (!is_adopting_pointer_template(primary_template_name(*constructor->getParent())))
        return true;
    mark_template_candidate();
    if (use_site_is_inside_ak())
        return true;

    auto& diag_engine = m_context.getDiagnostics();
    auto diag_id = diag_engine.getCustomDiagID(clang::DiagnosticsEngine::Error, "%0 is adopted without an allocator check. Use adopt_own(), adopt_ref() or make() instead");
    diag_engine.Report(construct_expression->getBeginLoc(), diag_id) << constructor->getParent();
    return true;
}

bool KmallocAllocationVisitor::VisitCXXUnresolvedConstructExpr(clang::CXXUnresolvedConstructExpr* construct_expression)
{
    if (m_unevaluated_depth > 0)
        return true;
    auto const* specialization = construct_expression->getTypeAsWritten()->getAs<clang::TemplateSpecializationType>();
    if (!specialization)
        return true;
    if (auto const* template_decl = specialization->getTemplateName().getAsTemplateDecl(); template_decl && is_adopting_pointer_template(template_decl->getQualifiedNameAsString()))
        mark_template_candidate();
    return true;
}

bool KmallocAllocationVisitor::VisitDependentScopeDeclRefExpr(clang::DependentScopeDeclRefExpr* reference)
{
    if (m_unevaluated_depth == 0 && reference->getDeclName().isIdentifier() && reference->getDeclName().getAsIdentifierInfo()->getName() == "lift")
        mark_template_candidate();
    return true;
}

bool KmallocAllocationVisitor::VisitCallExpr(clang::CallExpr* call_expression)
{
    if (m_unevaluated_depth > 0)
        return true;

    auto const* method = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(call_expression->getDirectCallee());
    if (!method || !method->getDeclName().isIdentifier() || method->getName() != "lift")
        return true;
    if (primary_template_name(*method->getParent()) != "AK::OwnPtr")
        return true;
    mark_template_candidate();
    if (use_site_is_inside_ak())
        return true;

    auto& diag_engine = m_context.getDiagnostics();
    auto diag_id = diag_engine.getCustomDiagID(clang::DiagnosticsEngine::Error, "%0 is lifted without an allocator check. Use adopt_own_if_nonnull() instead");
    diag_engine.Report(call_expression->getBeginLoc(), diag_id) << method->getParent();
    return true;
}

bool KmallocAllocationVisitor::VisitCXXRecordDecl(clang::CXXRecordDecl* record)
{
    if (!record->isCompleteDefinition() || record->isLambda())
        return true;
    if (record_declares_kmalloc_tag(*record))
        return true;

    for (auto const* method : record->methods()) {
        auto overloaded_operator = method->getOverloadedOperator();
        bool is_allocation_operator = overloaded_operator == clang::OO_New || overloaded_operator == clang::OO_Delete
            || overloaded_operator == clang::OO_Array_New || overloaded_operator == clang::OO_Array_Delete;
        if (!is_allocation_operator || method->getParent() != record)
            continue;
        mark_template_candidate();
        if (!is_project_record(*record) || !record_has_kmalloc_tag(*record))
            return true;

        auto& diag_engine = m_context.getDiagnostics();
        auto diag_id = diag_engine.getCustomDiagID(clang::DiagnosticsEngine::Error, "%0 inherits kmalloc allocation but declares its own operator %1");
        diag_engine.Report(method->getLocation(), diag_id) << record << clang::getOperatorSpelling(overloaded_operator);
    }
    return true;
}

void KmallocAllocationASTConsumer::HandleTranslationUnit(clang::ASTContext& context)
{
    KmallocAllocationVisitor visitor(context);
    visitor.collect_allocator_opt_outs();
    visitor.TraverseDecl(context.getTranslationUnitDecl());
}

static clang::FrontendPluginRegistry::Add<KmallocAllocationPluginAction> X("kmalloc_allocation", "check that project types allocate with kmalloc");
