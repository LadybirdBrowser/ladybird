/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2025, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LibJSGCPluginAction.h"
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendPluginRegistry.h>
#include <clang/Lex/MacroArgs.h>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

template<typename T>
class SimpleCollectMatchesCallback : public clang::ast_matchers::MatchFinder::MatchCallback {
public:
    explicit SimpleCollectMatchesCallback(std::string name)
        : m_name(std::move(name))
    {
    }

    void run(clang::ast_matchers::MatchFinder::MatchResult const& result) override
    {
        if (auto const* node = result.Nodes.getNodeAs<T>(m_name))
            m_matches.push_back(node);
    }

    auto const& matches() const { return m_matches; }

private:
    std::string m_name;
    std::vector<T const*> m_matches;
};

static bool record_inherits_from_cell(clang::CXXRecordDecl const& record)
{
    if (!record.isCompleteDefinition())
        return false;

    bool inherits_from_cell = record.getQualifiedNameAsString() == "GC::Cell";
    record.forallBases([&](clang::CXXRecordDecl const* base) -> bool {
        if (base->getQualifiedNameAsString() == "GC::Cell") {
            inherits_from_cell = true;
            return false;
        }
        return true;
    });
    return inherits_from_cell;
}

// Check if a type has a visit_edges method that takes GC::Cell::Visitor&
static bool type_has_visit_edges_method(clang::CXXRecordDecl const* record)
{
    if (!record || !record->isCompleteDefinition())
        return false;

    for (auto const* method : record->methods()) {
        if (method->getNameAsString() != "visit_edges")
            continue;
        if (method->getNumParams() != 1)
            continue;
        // Check that the parameter is a reference to Visitor (GC::Cell::Visitor)
        auto param_type = method->getParamDecl(0)->getType();
        if (!param_type->isReferenceType())
            continue;
        return true;
    }
    return false;
}

enum class ContainsGCPtrResult {
    No,
    Yes,
    YesRequiresVisitEdges, // Contains GC pointers and the type needs its own visit_edges
};

static std::map<clang::CXXRecordDecl const*, ContainsGCPtrResult> s_contains_gc_ptr_cache;

// Forward declaration
static ContainsGCPtrResult type_contains_gc_ptr(clang::QualType const& type, std::set<clang::CXXRecordDecl const*>& visited);

static ContainsGCPtrResult record_contains_gc_ptr(clang::CXXRecordDecl const* record, std::set<clang::CXXRecordDecl const*>& visited)
{
    if (!record || !record->isCompleteDefinition())
        return ContainsGCPtrResult::No;

    // Avoid infinite recursion
    if (visited.contains(record))
        return ContainsGCPtrResult::No;
    visited.insert(record);

    // Check cache
    auto cache_it = s_contains_gc_ptr_cache.find(record);
    if (cache_it != s_contains_gc_ptr_cache.end())
        return cache_it->second;

    // Cell types are visited directly via GC::Ptr/GC::Ref, not through substruct visiting
    if (record_inherits_from_cell(*record)) {
        s_contains_gc_ptr_cache[record] = ContainsGCPtrResult::No;
        return ContainsGCPtrResult::No;
    }

    // Skip GC infrastructure types that handle their own visiting
    auto qualified_name = record->getQualifiedNameAsString();
    static std::set<std::string> gc_infrastructure_types {
        "GC::Root",
        "GC::RootImpl",
        "GC::HeapBlock",
        "GC::CellAllocator",
        "GC::TypeIsolatingCellAllocator",
        "GC::RootVector",
        "GC::Heap",
        "GC::MarkedVector",
        "GC::ConservativeVector",
    };
    if (gc_infrastructure_types.contains(qualified_name)) {
        s_contains_gc_ptr_cache[record] = ContainsGCPtrResult::No;
        return ContainsGCPtrResult::No;
    }

    // Skip AK types - they're library types that don't need visit_edges
    if (qualified_name.starts_with("AK::") || qualified_name.starts_with("Optional<")) {
        s_contains_gc_ptr_cache[record] = ContainsGCPtrResult::No;
        return ContainsGCPtrResult::No;
    }

    ContainsGCPtrResult result = ContainsGCPtrResult::No;

    for (auto const* field : record->fields()) {
        auto field_result = type_contains_gc_ptr(field->getType(), visited);
        if (field_result == ContainsGCPtrResult::YesRequiresVisitEdges) {
            result = ContainsGCPtrResult::YesRequiresVisitEdges;
            break;
        }
        if (field_result == ContainsGCPtrResult::Yes) {
            result = ContainsGCPtrResult::YesRequiresVisitEdges;
        }
    }

    s_contains_gc_ptr_cache[record] = result;
    return result;
}

static ContainsGCPtrResult type_contains_gc_ptr(clang::QualType const& type, std::set<clang::CXXRecordDecl const*>& visited)
{
    // Handle elaborated types
    clang::QualType actual_type = type;
    if (auto const* elaborated = llvm::dyn_cast<clang::ElaboratedType>(type.getTypePtr()))
        actual_type = elaborated->desugar();

    // Check for JS::Value directly
    if (auto const* record = actual_type->getAsCXXRecordDecl()) {
        if (record->getQualifiedNameAsString() == "JS::Value")
            return ContainsGCPtrResult::Yes;
    }

    // Check for raw pointers to Cell types (these should use GC::Ptr instead)
    if (auto const* pointer_type = actual_type->getAs<clang::PointerType>()) {
        if (auto const* pointee = pointer_type->getPointeeCXXRecordDecl()) {
            if (pointee->hasDefinition() && record_inherits_from_cell(*pointee))
                return ContainsGCPtrResult::Yes;
        }
    }

    // Check for raw references to Cell types (these should use GC::Ref instead)
    if (auto const* reference_type = actual_type->getAs<clang::ReferenceType>()) {
        if (auto const* pointee = reference_type->getPointeeCXXRecordDecl()) {
            if (pointee->hasDefinition() && record_inherits_from_cell(*pointee))
                return ContainsGCPtrResult::Yes;
        }
    }

    // Check for template specializations (GC::Ptr, GC::Ref, Vector, HashMap, etc.)
    if (auto const* specialization = actual_type->getAs<clang::TemplateSpecializationType>()) {
        auto template_name = specialization->getTemplateName().getAsTemplateDecl()->getQualifiedNameAsString();

        // Direct GC pointer types
        if (template_name == "GC::Ptr" || template_name == "GC::Ref")
            return ContainsGCPtrResult::Yes;

        // Raw pointers don't need visiting
        if (template_name == "GC::RawPtr" || template_name == "GC::RawRef")
            return ContainsGCPtrResult::No;

        // Root types handle their own visiting
        if (template_name == "GC::Root" || template_name == "GC::RootVector")
            return ContainsGCPtrResult::No;

        // Check template arguments recursively for containers
        for (auto const& arg : specialization->template_arguments()) {
            if (arg.getKind() == clang::TemplateArgument::Type) {
                auto arg_result = type_contains_gc_ptr(arg.getAsType(), visited);
                if (arg_result != ContainsGCPtrResult::No)
                    return arg_result;
            }
        }
    }

    // Check for record types (structs/classes) that might contain GC pointers
    if (auto const* record = actual_type->getAsCXXRecordDecl()) {
        return record_contains_gc_ptr(record, visited);
    }

    return ContainsGCPtrResult::No;
}

static ContainsGCPtrResult type_contains_gc_ptr(clang::QualType const& type)
{
    std::set<clang::CXXRecordDecl const*> visited;
    return type_contains_gc_ptr(type, visited);
}

static std::vector<clang::QualType> get_all_qualified_types(clang::QualType const& type)
{
    std::vector<clang::QualType> qualified_types;

    if (auto const* template_specialization = type->getAs<clang::TemplateSpecializationType>()) {
        auto specialization_name = template_specialization->getTemplateName().getAsTemplateDecl()->getQualifiedNameAsString();
        // Do not unwrap GCPtr/NonnullGCPtr/RootVector
        static std::unordered_set<std::string> gc_relevant_type_names {
            "GC::Ptr",
            "GC::Ref",
            "GC::RawPtr",
            "GC::RawRef",
            "GC::RootVector",
            "GC::Root",
        };

        if (gc_relevant_type_names.contains(specialization_name)) {
            qualified_types.push_back(type);
        } else {
            auto const template_arguments = template_specialization->template_arguments();
            for (size_t i = 0; i < template_arguments.size(); i++) {
                auto const& template_arg = template_arguments[i];
                if (template_arg.getKind() == clang::TemplateArgument::Type) {
                    auto template_qualified_types = get_all_qualified_types(template_arg.getAsType());
                    std::move(template_qualified_types.begin(), template_qualified_types.end(), std::back_inserter(qualified_types));
                }
            }
        }
    } else {
        qualified_types.push_back(type);
    }

    return qualified_types;
}
enum class OuterType {
    GCPtr,
    RawPtr,
    Root,
    Ptr,
    Ref,
    Value,
};

struct QualTypeGCInfo {
    std::optional<OuterType> outer_type { {} };
    bool base_type_inherits_from_cell { false };
};

static std::optional<QualTypeGCInfo> validate_qualified_type(clang::QualType const& type)
{
    if (auto const* pointer_decl = type->getAs<clang::PointerType>()) {
        if (auto const* pointee = pointer_decl->getPointeeCXXRecordDecl())
            return QualTypeGCInfo { OuterType::Ptr, record_inherits_from_cell(*pointee) };
    } else if (auto const* reference_decl = type->getAs<clang::ReferenceType>()) {
        if (auto const* pointee = reference_decl->getPointeeCXXRecordDecl())
            return QualTypeGCInfo { OuterType::Ref, record_inherits_from_cell(*pointee) };
    } else if (auto const* specialization = type->getAs<clang::TemplateSpecializationType>()) {
        auto template_type_name = specialization->getTemplateName().getAsTemplateDecl()->getQualifiedNameAsString();

        OuterType outer_type;
        if (template_type_name == "GC::Ptr" || template_type_name == "GC::Ref") {
            outer_type = OuterType::GCPtr;
        } else if (template_type_name == "GC::RawPtr" || template_type_name == "GC::RawRef") {
            outer_type = OuterType::RawPtr;
        } else if (template_type_name == "GC::Root") {
            outer_type = OuterType::Root;
        } else {
            return {};
        }

        auto template_args = specialization->template_arguments();
        if (template_args.size() != 1)
            return {}; // Not really valid, but will produce a compilation error anyway

        auto const& type_arg = template_args[0];
        auto const* record_type = type_arg.getAsType()->getAs<clang::RecordType>();
        if (!record_type)
            return {};

        auto const* record_decl = record_type->getAsCXXRecordDecl();
        if (!record_decl->hasDefinition()) {
            // If we don't have a definition (this is a forward declaration), assume that the type inherits from
            // GC::Cell instead of not checking it at all. If it does inherit from GC:Cell, this will make sure it's
            // visited. If it does not, any attempt to visit it will fail compilation on the visit call itself,
            // ensuring it's no longer wrapped in a GC::Ptr.
            return QualTypeGCInfo { outer_type, true };
        }

        return QualTypeGCInfo { outer_type, record_inherits_from_cell(*record_decl) };
    } else if (auto const* record = type->getAsCXXRecordDecl()) {
        if (record->getQualifiedNameAsString() == "JS::Value")
            return QualTypeGCInfo { OuterType::Value, true };
    }

    return {};
}

static std::optional<QualTypeGCInfo> validate_field_qualified_type(clang::FieldDecl const* field_decl)
{
    auto type = field_decl->getType();
    if (auto const* elaborated_type = llvm::dyn_cast<clang::ElaboratedType>(type.getTypePtr()))
        type = elaborated_type->desugar();

    for (auto const& qualified_type : get_all_qualified_types(type)) {
        if (auto error = validate_qualified_type(qualified_type))
            return error;
    }

    return {};
}

static bool decl_has_annotation(clang::Decl const* decl, std::string name)
{
    for (auto const* attr : decl->attrs()) {
        if (auto const* annotate_attr = llvm::dyn_cast<clang::AnnotateAttr>(attr)) {
            if (annotate_attr->getAnnotation() == name)
                return true;
        }
    }
    return false;
}

bool LibJSGCVisitor::VisitCXXRecordDecl(clang::CXXRecordDecl* record)
{
    using namespace clang::ast_matchers;

    if (!record || !record->isCompleteDefinition() || (!record->isClass() && !record->isStruct()))
        return true;

    // Cell triggers a bunch of warnings for its empty visit_edges implementation, but
    // it doesn't have any members anyways so it's fine to just ignore.
    auto qualified_name = record->getQualifiedNameAsString();
    if (qualified_name == "GC::Cell")
        return true;

    auto& diag_engine = m_context.getDiagnostics();
    std::vector<clang::FieldDecl const*> fields_that_need_visiting;
    std::vector<clang::FieldDecl const*> substruct_fields_that_need_visiting;
    auto record_is_cell = record_inherits_from_cell(*record);

    for (clang::FieldDecl const* field : record->fields()) {
        if (decl_has_annotation(field, "serenity::ignore_gc"))
            continue;

        // Skip anonymous structs/unions - their members are accessed indirectly
        // and may be handled specially (e.g., tagged unions with type checks)
        if (field->isAnonymousStructOrUnion())
            continue;

        auto validation_results = validate_field_qualified_type(field);

        if (validation_results) {
            auto [outer_type, base_type_inherits_from_cell] = *validation_results;

            if (outer_type == OuterType::Ptr || outer_type == OuterType::Ref) {
                if (base_type_inherits_from_cell) {
                    auto diag_id = diag_engine.getCustomDiagID(clang::DiagnosticsEngine::Error, "%0 to GC::Cell type should be wrapped in %1");
                    auto builder = diag_engine.Report(field->getLocation(), diag_id);
                    if (outer_type == OuterType::Ref) {
                        builder << "reference"
                                << "GC::Ref";
                    } else {
                        builder << "pointer"
                                << "GC::Ptr";
                    }
                }
            } else if (outer_type == OuterType::GCPtr || outer_type == OuterType::RawPtr || outer_type == OuterType::Value) {
                if (!base_type_inherits_from_cell) {
                    auto diag_id = diag_engine.getCustomDiagID(clang::DiagnosticsEngine::Error, "Specialization type must inherit from GC::Cell");
                    diag_engine.Report(field->getLocation(), diag_id);
                } else if (outer_type != OuterType::RawPtr) {
                    fields_that_need_visiting.push_back(field);
                }
            } else if (outer_type == OuterType::Root) {
                if (record_is_cell && m_detect_invalid_function_members) {
                    // FIXME: Change this to an Error when all of the use cases get addressed and remove the plugin argument
                    auto diag_id = diag_engine.getCustomDiagID(clang::DiagnosticsEngine::Warning, "Types inheriting from GC::Cell should not have %0 fields");
                    auto builder = diag_engine.Report(field->getLocation(), diag_id);
                    builder << "GC::Root";
                }
            }
            // Field is a direct GC type, don't also check for substruct
            continue;
        }

        // Check if this field is a substruct (non-Cell type) containing GC pointers
        auto contains_result = type_contains_gc_ptr(field->getType());
        if (contains_result == ContainsGCPtrResult::YesRequiresVisitEdges) {
            substruct_fields_that_need_visiting.push_back(field);
        }
    }

    // Non-Cell types don't need visit_edges just for existing - they only need it
    // when used as a member of a Cell (checked below for Cell types).
    // However, if they DO have visit_edges, verify it visits all GC members.
    if (!record_is_cell) {
        // Check if this non-Cell type has a visit_edges method
        clang::DeclarationName name = &m_context.Idents.get("visit_edges");
        auto const* visit_edges_method = record->lookup(name).find_first<clang::CXXMethodDecl>();
        if (visit_edges_method && visit_edges_method->getBody()) {
            // Verify that all GC pointer fields are visited
            if (!fields_that_need_visiting.empty() || !substruct_fields_that_need_visiting.empty()) {
                MatchFinder field_access_finder;
                SimpleCollectMatchesCallback<clang::MemberExpr> field_access_callback("member-expr");

                auto field_access_matcher = memberExpr(
                    hasAncestor(cxxMethodDecl(hasName("visit_edges"))),
                    hasObjectExpression(hasType(pointsTo(cxxRecordDecl(hasName(record->getName()))))))
                                                .bind("member-expr");

                field_access_finder.addMatcher(field_access_matcher, &field_access_callback);
                field_access_finder.matchAST(visit_edges_method->getASTContext());

                std::unordered_set<std::string> fields_that_are_visited;
                for (auto const* member_expr : field_access_callback.matches())
                    fields_that_are_visited.insert(member_expr->getMemberNameInfo().getAsString());

                auto gc_member_diag_id = diag_engine.getCustomDiagID(clang::DiagnosticsEngine::Error,
                    "GC-allocated member is not visited in %0::visit_edges");

                for (auto const* field : fields_that_need_visiting) {
                    if (!fields_that_are_visited.contains(field->getNameAsString())) {
                        auto builder = diag_engine.Report(field->getBeginLoc(), gc_member_diag_id);
                        builder << record->getName();
                    }
                }

                auto substruct_diag_id = diag_engine.getCustomDiagID(clang::DiagnosticsEngine::Error,
                    "Member %0 contains GC pointers but is not visited in %1::visit_edges");

                for (auto const* field : substruct_fields_that_need_visiting) {
                    if (!fields_that_are_visited.contains(field->getNameAsString())) {
                        auto builder = diag_engine.Report(field->getBeginLoc(), substruct_diag_id);
                        builder << field->getName() << record->getName();
                    }
                }
            }
        }
        return true;
    }

    validate_record_macros(*record);

    // Check that overrides of must_survive_garbage_collection() and finalize() have the
    // corresponding static constexpr bool flags set
    auto check_override_requires_flag = [&](char const* method_name, char const* flag_name) {
        clang::DeclarationName decl_name = &m_context.Idents.get(method_name);
        auto const* method = record->lookup(decl_name).find_first<clang::CXXMethodDecl>();
        if (!method || !method->isVirtual() || !method->size_overridden_methods())
            return;

        // Check if the method is defined in this class (not just inherited)
        if (method->getParent() != record)
            return;

        // Look for the static constexpr bool flag
        clang::DeclarationName flag_decl_name = &m_context.Idents.get(flag_name);
        auto const* flag_var = record->lookup(flag_decl_name).find_first<clang::VarDecl>();

        bool flag_found = false;
        if (flag_var && flag_var->isStaticDataMember() && flag_var->isConstexpr()) {
            // Check if it's set to true
            if (auto const* init = flag_var->getInit()) {
                if (auto const* bool_literal = llvm::dyn_cast<clang::CXXBoolLiteralExpr>(init->IgnoreParenImpCasts())) {
                    flag_found = bool_literal->getValue();
                }
            }
        }

        if (!flag_found) {
            auto diag_id = diag_engine.getCustomDiagID(clang::DiagnosticsEngine::Error,
                "Class %0 overrides %1 but does not set static constexpr bool %2 = true");
            auto builder = diag_engine.Report(method->getBeginLoc(), diag_id);
            builder << record->getName() << method_name << flag_name;
        }
    };

    check_override_requires_flag("must_survive_garbage_collection", "OVERRIDES_MUST_SURVIVE_GARBAGE_COLLECTION");
    check_override_requires_flag("finalize", "OVERRIDES_FINALIZE");

    // Check that Cell subclasses (and all their base classes) don't have non-trivial destructors.
    // They should override Cell::finalize() instead.
    auto check_no_nontrivial_destructor = [&](clang::CXXRecordDecl const* check_record) {
        if (!check_record || !check_record->isCompleteDefinition())
            return;
        if (check_record->getQualifiedNameAsString() == "GC::Cell")
            return;
        auto const* destructor = check_record->getDestructor();
        if (!destructor || !destructor->isUserProvided())
            return;
        // Only flag destructors whose body we can see, that aren't defaulted,
        // and that have a non-empty body. This way, out-of-line `= default` destructors
        // and empty-body destructors `~Foo() {}` are fine.
        if (!destructor->getBody() || destructor->isDefaulted())
            return;
        if (auto const* body = llvm::dyn_cast<clang::CompoundStmt>(destructor->getBody())) {
            if (body->body_empty())
                return;
        }
        if (decl_has_annotation(destructor, "ladybird::allow_cell_destructor"))
            return;
        auto diag_id = diag_engine.getCustomDiagID(clang::DiagnosticsEngine::Error,
            "GC::Cell-inheriting class %0 has a non-trivial destructor; override Cell::finalize() instead (and set OVERRIDES_FINALIZE)");
        auto builder = diag_engine.Report(destructor->getBeginLoc(), diag_id);
        builder << check_record->getName();
    };

    check_no_nontrivial_destructor(record);
    record->forallBases([&](clang::CXXRecordDecl const* base) -> bool {
        if (base->getQualifiedNameAsString() == "GC::Cell")
            return false;
        // Only check bases that are themselves part of the Cell hierarchy.
        // Non-Cell mixins (e.g. Weakable) are not our concern here.
        if (!record_inherits_from_cell(*base))
            return true;
        check_no_nontrivial_destructor(base);
        return true;
    });

    clang::DeclarationName name = &m_context.Idents.get("visit_edges");
    auto const* visit_edges_method = record->lookup(name).find_first<clang::CXXMethodDecl>();
    if (!visit_edges_method && !fields_that_need_visiting.empty()) {
        auto diag_id = diag_engine.getCustomDiagID(clang::DiagnosticsEngine::Error, "GC::Cell-inheriting class %0 contains a GC-allocated member %1 but has no visit_edges method");
        auto builder = diag_engine.Report(record->getLocation(), diag_id);
        builder << record->getName()
                << fields_that_need_visiting[0];
    }
    if (!visit_edges_method && !substruct_fields_that_need_visiting.empty()) {
        auto diag_id = diag_engine.getCustomDiagID(clang::DiagnosticsEngine::Error, "GC::Cell-inheriting class %0 contains a member %1 that has GC pointers but has no visit_edges method");
        auto builder = diag_engine.Report(record->getLocation(), diag_id);
        builder << record->getName()
                << substruct_fields_that_need_visiting[0];
    }
    if (!visit_edges_method || !visit_edges_method->getBody())
        return true;

    // NOTE: The check for calling Base::visit_edges() is now handled by the general
    // must_upcall attribute check in VisitCXXMethodDecl, since Cell::visit_edges()
    // is annotated with MUST_UPCALL.

    // Search for uses of all fields that need visiting. We don't ensure they are _actually_ visited
    // with a call to visitor.visit(...), as that is too complex. Instead, we just assume that if the
    // field is accessed at all, then it is visited.

    if (fields_that_need_visiting.empty() && substruct_fields_that_need_visiting.empty())
        return true;

    MatchFinder field_access_finder;
    SimpleCollectMatchesCallback<clang::MemberExpr> field_access_callback("member-expr");

    auto field_access_matcher = memberExpr(
        hasAncestor(cxxMethodDecl(hasName("visit_edges"))),
        hasObjectExpression(hasType(pointsTo(cxxRecordDecl(hasName(record->getName()))))))
                                    .bind("member-expr");

    field_access_finder.addMatcher(field_access_matcher, &field_access_callback);
    field_access_finder.matchAST(visit_edges_method->getASTContext());

    std::unordered_set<std::string> fields_that_are_visited;
    for (auto const* member_expr : field_access_callback.matches())
        fields_that_are_visited.insert(member_expr->getMemberNameInfo().getAsString());

    auto gc_member_diag_id = diag_engine.getCustomDiagID(clang::DiagnosticsEngine::Error, "GC-allocated member is not visited in %0::visit_edges");

    for (auto const* field : fields_that_need_visiting) {
        if (!fields_that_are_visited.contains(field->getNameAsString())) {
            auto builder = diag_engine.Report(field->getBeginLoc(), gc_member_diag_id);
            builder << record->getName();
        }
    }

    auto substruct_not_visited_diag_id = diag_engine.getCustomDiagID(clang::DiagnosticsEngine::Error,
        "Member %0 contains GC pointers but is not visited in %1::visit_edges");
    auto substruct_needs_visit_edges_diag_id = diag_engine.getCustomDiagID(clang::DiagnosticsEngine::Error,
        "Member %0 contains GC pointers but its type has no visit_edges method");

    for (auto const* field : substruct_fields_that_need_visiting) {
        if (!fields_that_are_visited.contains(field->getNameAsString())) {
            // Check if the substruct type has a visit_edges method
            auto field_type = field->getType();
            if (auto const* elaborated = llvm::dyn_cast<clang::ElaboratedType>(field_type.getTypePtr()))
                field_type = elaborated->desugar();

            // For smart pointer types (OwnPtr, RefPtr, etc.), check the pointed-to type
            clang::CXXRecordDecl const* type_to_check = nullptr;
            if (auto const* specialization = field_type->getAs<clang::TemplateSpecializationType>()) {
                auto template_name = specialization->getTemplateName().getAsTemplateDecl()->getQualifiedNameAsString();
                static std::set<std::string> smart_pointer_types {
                    "OwnPtr", "NonnullOwnPtr", "RefPtr", "NonnullRefPtr",
                    "ValueComparingRefPtr", "ValueComparingNonnullRefPtr",
                    "AK::OwnPtr", "AK::NonnullOwnPtr", "AK::RefPtr", "AK::NonnullRefPtr",
                    "Web::CSS::ValueComparingRefPtr", "Web::CSS::ValueComparingNonnullRefPtr"
                };
                if (smart_pointer_types.contains(template_name)) {
                    auto const& args = specialization->template_arguments();
                    if (args.size() >= 1 && args[0].getKind() == clang::TemplateArgument::Type) {
                        type_to_check = args[0].getAsType()->getAsCXXRecordDecl();
                    }
                }
            }
            if (!type_to_check)
                type_to_check = field_type->getAsCXXRecordDecl();

            if (type_to_check && !type_has_visit_edges_method(type_to_check)) {
                auto builder = diag_engine.Report(field->getBeginLoc(), substruct_needs_visit_edges_diag_id);
                builder << field->getName();
            } else {
                auto builder = diag_engine.Report(field->getBeginLoc(), substruct_not_visited_diag_id);
                builder << field->getName() << record->getName();
            }
        }
    }

    return true;
}

// Check if a method (or any method it overrides) has the must_upcall annotation
static bool method_requires_upcall(clang::CXXMethodDecl const* method)
{
    if (!method)
        return false;

    if (decl_has_annotation(method, "must_upcall"))
        return true;

    // Check overridden methods recursively
    for (auto const* overridden : method->overridden_methods()) {
        if (method_requires_upcall(overridden))
            return true;
    }

    return false;
}

// Get the immediate parent class's method that this method overrides
static clang::CXXMethodDecl const* get_immediate_base_method(clang::CXXMethodDecl const* method)
{
    if (!method->isVirtual() || method->overridden_methods().empty())
        return nullptr;

    // The overridden_methods() returns the immediate parent(s) that this method overrides
    // For single inheritance, there's just one
    for (auto const* overridden : method->overridden_methods())
        return overridden;

    return nullptr;
}

bool LibJSGCVisitor::VisitCXXMethodDecl(clang::CXXMethodDecl* method)
{
    if (!method || !method->isVirtual() || !method->doesThisDeclarationHaveABody())
        return true;

    // Skip if this method is not an override
    if (!method->size_overridden_methods())
        return true;

    // Check if any method in the override chain has must_upcall annotation
    if (!method_requires_upcall(method))
        return true;

    auto const* base_method = get_immediate_base_method(method);
    if (!base_method)
        return true;

    auto const* parent_class = base_method->getParent();
    if (!parent_class)
        return true;

    auto method_name = method->getNameAsString();

    // Search for a call to Base::method_name or ParentClass::method_name
    using namespace clang::ast_matchers;

    MatchFinder upcall_finder;
    SimpleCollectMatchesCallback<clang::MemberExpr> upcall_callback("member-call");

    auto upcall_matcher = cxxMethodDecl(
        equalsNode(method),
        hasDescendant(memberExpr(member(hasName(method_name))).bind("member-call")));

    upcall_finder.addMatcher(upcall_matcher, &upcall_callback);
    upcall_finder.matchAST(m_context);

    bool upcall_found = false;

    for (auto const* member_expr : upcall_callback.matches()) {
        // Check if this is a qualified call (e.g., Base::method or ParentClass::method)
        if (!member_expr->hasQualifier())
            continue;

        // Get the record decl that the qualifier refers to
        auto const* qualifier = member_expr->getQualifier();
        if (!qualifier)
            continue;

        auto const* qualifier_type = qualifier->getAsType();
        if (!qualifier_type)
            continue;

        auto const* qualifier_record = qualifier_type->getAsCXXRecordDecl();
        if (!qualifier_record)
            continue;

        // Check if the qualifier refers to a base class of the current class
        auto const* current_class = method->getParent();
        if (!current_class)
            continue;

        // The qualifier should be the same as or a base of the parent class
        if (qualifier_record == parent_class || current_class->isDerivedFrom(qualifier_record)) {
            upcall_found = true;
            break;
        }
    }

    if (!upcall_found) {
        auto& diag_engine = m_context.getDiagnostics();
        auto diag_id = diag_engine.getCustomDiagID(clang::DiagnosticsEngine::Error,
            "Missing call to Base::%0 (required by must_upcall attribute)");
        auto builder = diag_engine.Report(method->getBeginLoc(), diag_id);
        builder << method_name;
    }

    return true;
}

struct CellTypeWithOrigin {
    clang::CXXRecordDecl const& base_origin;
    LibJSCellMacro::Type type;
};

static std::optional<CellTypeWithOrigin> find_cell_type_with_origin(clang::CXXRecordDecl const& record)
{
    for (auto const& base : record.bases()) {
        if (auto const* base_record = base.getType()->getAsCXXRecordDecl()) {
            auto base_name = base_record->getQualifiedNameAsString();

            if (base_name == "GC::Cell")
                return CellTypeWithOrigin { *base_record, LibJSCellMacro::Type::GCCell };

            if (base_name == "GC::ForeignCell")
                return CellTypeWithOrigin { *base_record, LibJSCellMacro::Type::ForeignCell };

            if (base_name == "JS::Object")
                return CellTypeWithOrigin { *base_record, LibJSCellMacro::Type::JSObject };

            if (base_name == "JS::Environment")
                return CellTypeWithOrigin { *base_record, LibJSCellMacro::Type::JSEnvironment };

            if (base_name == "JS::PrototypeObject")
                return CellTypeWithOrigin { *base_record, LibJSCellMacro::Type::JSPrototypeObject };

            if (base_name == "Web::Bindings::PlatformObject")
                return CellTypeWithOrigin { *base_record, LibJSCellMacro::Type::WebPlatformObject };

            if (auto origin = find_cell_type_with_origin(*base_record))
                return CellTypeWithOrigin { *base_record, origin->type };
        }
    }

    return {};
}

LibJSGCVisitor::CellMacroExpectation LibJSGCVisitor::get_record_cell_macro_expectation(clang::CXXRecordDecl const& record)
{
    if (record.getQualifiedNameAsString() == "GC::ForeignCell")
        return { LibJSCellMacro::Type::ForeignCell, "Cell" };

    auto origin = find_cell_type_with_origin(record);
    assert(origin.has_value());

    // Need to iterate the bases again to turn the record into the exact text that the user used as
    // the class base, since it doesn't have to be qualified (but might be).
    for (auto const& base : record.bases()) {
        if (auto const* base_record = base.getType()->getAsCXXRecordDecl()) {
            if (base_record == &origin->base_origin) {
                auto& source_manager = m_context.getSourceManager();
                auto char_range = source_manager.getExpansionRange({ base.getBaseTypeLoc(), base.getEndLoc() });
                auto exact_text = clang::Lexer::getSourceText(char_range, source_manager, m_context.getLangOpts());
                return { origin->type, exact_text.str() };
            }
        }
    }

    assert(false);
    __builtin_unreachable();
}

void LibJSGCVisitor::validate_record_macros(clang::CXXRecordDecl const& record)
{
    auto& source_manager = m_context.getSourceManager();
    auto record_range = record.getSourceRange();

    // FIXME: The current macro detection doesn't recursively search through macro expansion,
    //        so if the record itself is defined in a macro, the GC_CELL/etc won't be found
    if (source_manager.isMacroBodyExpansion(record_range.getBegin()))
        return;

    auto [expected_cell_macro_type, expected_base_name] = get_record_cell_macro_expectation(record);
    auto file_id = m_context.getSourceManager().getFileID(record.getLocation());
    auto it = m_macro_map.find(file_id.getHashValue());
    auto& diag_engine = m_context.getDiagnostics();

    auto report_missing_macro = [&] {
        auto diag_id = diag_engine.getCustomDiagID(clang::DiagnosticsEngine::Error, "Expected record to have a %0 macro invocation");
        auto builder = diag_engine.Report(record.getLocation(), diag_id);
        builder << LibJSCellMacro::type_name(expected_cell_macro_type);
    };

    if (it == m_macro_map.end()) {
        report_missing_macro();
        return;
    }

    std::vector<clang::SourceRange> sub_ranges;
    for (auto const& sub_decl : record.decls()) {
        if (auto const* sub_record = llvm::dyn_cast<clang::CXXRecordDecl>(sub_decl))
            sub_ranges.push_back(sub_record->getSourceRange());
    }

    bool found_macro = false;

    auto record_name = record.getDeclName().getAsString();
    if (record.getQualifier()) {
        // FIXME: There has to be a better way to get this info. getQualifiedNameAsString() gets too much info
        //        (outer namespaces that aren't part of the class identifier), and getNameAsString() doesn't get
        //        enough info (doesn't include parts before the namespace specifier).
        auto loc = record.getQualifierLoc();
        auto& sm = m_context.getSourceManager();
        auto begin_offset = sm.getFileOffset(loc.getBeginLoc());
        auto end_offset = sm.getFileOffset(loc.getEndLoc());
        auto const* file_buf = sm.getCharacterData(loc.getBeginLoc());
        auto prefix = std::string { file_buf, end_offset - begin_offset };
        record_name = prefix + "::" + record_name;
    }

    for (auto const& macro : it->second) {
        if (record_range.fullyContains(macro.range)) {
            bool macro_is_in_sub_decl = false;
            for (auto const& sub_range : sub_ranges) {
                if (sub_range.fullyContains(macro.range)) {
                    macro_is_in_sub_decl = true;
                    break;
                }
            }
            if (macro_is_in_sub_decl)
                continue;

            if (found_macro) {
                auto diag_id = diag_engine.getCustomDiagID(clang::DiagnosticsEngine::Error, "Record has multiple GC_CELL-like macro invocations");
                diag_engine.Report(record_range.getBegin(), diag_id);
            }

            found_macro = true;
            if (macro.type != expected_cell_macro_type) {
                auto diag_id = diag_engine.getCustomDiagID(clang::DiagnosticsEngine::Error, "Invalid GC-CELL-like macro invocation; expected %0");
                auto builder = diag_engine.Report(macro.range.getBegin(), diag_id);
                builder << LibJSCellMacro::type_name(expected_cell_macro_type);
            }

            // This is a compile error, no diagnostic needed
            if (macro.args.size() < 2)
                return;

            // NOTE: DOMURL is a special case since the C++ class is named differently than the IDL.
            if (macro.args[0].text != record_name && record_name != "DOMURL") {
                auto diag_id = diag_engine.getCustomDiagID(clang::DiagnosticsEngine::Error, "Expected first argument of %0 macro invocation to be %1");
                auto builder = diag_engine.Report(macro.args[0].location, diag_id);
                builder << LibJSCellMacro::type_name(expected_cell_macro_type) << record_name;
            }

            if (expected_cell_macro_type == LibJSCellMacro::Type::JSPrototypeObject) {
                // FIXME: Validate the args for this macro
            } else if (macro.args[1].text != expected_base_name) {
                auto diag_id = diag_engine.getCustomDiagID(clang::DiagnosticsEngine::Error, "Expected second argument of %0 macro invocation to be %1");
                auto builder = diag_engine.Report(macro.args[1].location, diag_id);
                builder << LibJSCellMacro::type_name(expected_cell_macro_type) << expected_base_name;
            }
        }
    }

    if (!found_macro)
        report_missing_macro();
}

LibJSGCASTConsumer::LibJSGCASTConsumer(clang::CompilerInstance& compiler, bool detect_invalid_function_members)
    : m_compiler(compiler)
    , m_detect_invalid_function_members(detect_invalid_function_members)
{
    auto& preprocessor = compiler.getPreprocessor();
    preprocessor.addPPCallbacks(std::make_unique<LibJSPPCallbacks>(preprocessor, m_macro_map));
}

void LibJSGCASTConsumer::HandleTranslationUnit(clang::ASTContext& context)
{
    LibJSGCVisitor visitor { context, m_macro_map, m_detect_invalid_function_members };
    visitor.TraverseDecl(context.getTranslationUnitDecl());
}

char const* LibJSCellMacro::type_name(Type type)
{
    switch (type) {
    case Type::GCCell:
        return "GC_CELL";
    case Type::ForeignCell:
        return "FOREIGN_CELL";
    case Type::JSObject:
        return "JS_OBJECT";
    case Type::JSEnvironment:
        return "JS_ENVIRONMENT";
    case Type::JSPrototypeObject:
        return "JS_PROTOTYPE_OBJECT";
    case Type::WebPlatformObject:
        return "WEB_PLATFORM_OBJECT";
    default:
        __builtin_unreachable();
    }
}

void LibJSPPCallbacks::LexedFileChanged(clang::FileID curr_fid, LexedFileChangeReason reason, clang::SrcMgr::CharacteristicKind, clang::FileID, clang::SourceLocation)
{
    if (reason == LexedFileChangeReason::EnterFile) {
        m_curr_fid_hash_stack.push_back(curr_fid.getHashValue());
    } else {
        assert(!m_curr_fid_hash_stack.empty());
        m_curr_fid_hash_stack.pop_back();
    }
}

void LibJSPPCallbacks::MacroExpands(clang::Token const& name_token, clang::MacroDefinition const&, clang::SourceRange range, clang::MacroArgs const* args)
{
    if (auto* ident_info = name_token.getIdentifierInfo()) {
        static llvm::StringMap<LibJSCellMacro::Type> libjs_macro_types {
            { "GC_CELL", LibJSCellMacro::Type::GCCell },
            { "FOREIGN_CELL", LibJSCellMacro::Type::ForeignCell },
            { "JS_OBJECT", LibJSCellMacro::Type::JSObject },
            { "JS_ENVIRONMENT", LibJSCellMacro::Type::JSEnvironment },
            { "JS_PROTOTYPE_OBJECT", LibJSCellMacro::Type::JSPrototypeObject },
            { "WEB_PLATFORM_OBJECT", LibJSCellMacro::Type::WebPlatformObject },
            { "WEB_NON_IDL_PLATFORM_OBJECT", LibJSCellMacro::Type::WebPlatformObject },
        };

        auto name = ident_info->getName();
        if (auto it = libjs_macro_types.find(name); it != libjs_macro_types.end()) {
            LibJSCellMacro macro { range, it->second, {} };

            for (size_t arg_index = 0; arg_index < args->getNumMacroArguments(); arg_index++) {
                auto const* first_token = args->getUnexpArgument(arg_index);
                auto stringified_token = clang::MacroArgs::StringifyArgument(first_token, m_preprocessor, false, range.getBegin(), range.getEnd());
                // The token includes leading and trailing quotes
                auto len = strlen(stringified_token.getLiteralData());
                std::string arg_text { stringified_token.getLiteralData() + 1, len - 2 };
                macro.args.push_back({ arg_text, first_token->getLocation() });
            }

            assert(!m_curr_fid_hash_stack.empty());
            auto curr_fid_hash = m_curr_fid_hash_stack.back();
            if (m_macro_map.find(curr_fid_hash) == m_macro_map.end())
                m_macro_map[curr_fid_hash] = {};
            m_macro_map[curr_fid_hash].push_back(macro);
        }
    }
}

static clang::FrontendPluginRegistry::Add<LibJSGCPluginAction> X("libjs_gc_scanner", "analyze LibJS GC usage");
