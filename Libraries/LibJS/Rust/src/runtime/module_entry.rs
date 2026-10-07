/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::Utf16FlyString;

pub use crate::ast::ExportEntryKind;
use crate::runtime::module_request::ModuleRequest;

// https://tc39.es/ecma262/#table-importentry-record-fields
#[derive(Clone)]
pub struct ImportEntry {
    pub import_name: Option<Utf16FlyString>, // [[ImportName]]: stored string if Optional is not empty, NAMESPACE-OBJECT otherwise
    pub local_name: Utf16FlyString,          // [[LocalName]]
    pub module_request: Option<ModuleRequest>, // [[ModuleRequest]]
}

impl ImportEntry {
    pub fn is_namespace(&self) -> bool {
        self.import_name.is_none()
    }

    pub fn module_request(&self) -> &ModuleRequest {
        self.module_request
            .as_ref()
            .expect("an import entry has a module request")
    }
}

// ExportEntry Record, https://tc39.es/ecma262/#table-exportentry-records
// NB: ExportEntryKind::EmptyNamedExport is a special kind for export {} from "module", which should import the module
//     without getting any of the exports, without giving it a fake export name that may be a duplicate.
#[derive(Clone)]
pub struct ExportEntry {
    pub kind: ExportEntryKind,
    pub export_name: Option<Utf16FlyString>,          // [[ExportName]]
    pub local_or_import_name: Option<Utf16FlyString>, // Either [[ImportName]] or [[LocalName]]
    pub module_request: Option<ModuleRequest>,        // [[ModuleRequest]]
}

impl ExportEntry {
    pub fn is_module_request(&self) -> bool {
        self.module_request.is_some()
    }

    pub fn module_request(&self) -> &ModuleRequest {
        self.module_request
            .as_ref()
            .expect("an export entry from another module has a module request")
    }
}
