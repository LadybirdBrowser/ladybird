/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The GL objects of a context by the ids that WebContent gave them.

use core::ffi::c_void;
use std::collections::HashMap;

use super::replay::ReplayError;

#[derive(Default)]
pub struct ObjectMap {
    objects: HashMap<u32, u32>,
    syncs: HashMap<u32, *mut c_void>,
}

fn add_unique<V>(map: &mut HashMap<u32, V>, id: u32, value: V) -> Result<(), ReplayError> {
    if id == 0 {
        return Err(ReplayError("WebGL object id 0 is reserved"));
    }
    if map.contains_key(&id) {
        return Err(ReplayError("WebGL object id is already in use"));
    }
    map.insert(id, value);
    Ok(())
}

impl ObjectMap {
    /// 0 for an id that names no object.
    pub fn lookup(&self, id: u32) -> u32 {
        self.objects.get(&id).copied().unwrap_or(0)
    }

    pub fn take(&mut self, id: u32) -> u32 {
        self.objects.remove(&id).unwrap_or(0)
    }

    pub fn add(&mut self, id: u32, name: u32) -> Result<(), ReplayError> {
        add_unique(&mut self.objects, id, name)
    }

    pub fn lookup_sync(&self, id: u32) -> *mut c_void {
        self.syncs.get(&id).copied().unwrap_or(core::ptr::null_mut())
    }

    pub fn take_sync(&mut self, id: u32) -> *mut c_void {
        self.syncs.remove(&id).unwrap_or(core::ptr::null_mut())
    }

    pub fn add_sync(&mut self, id: u32, sync: *mut c_void) -> Result<(), ReplayError> {
        add_unique(&mut self.syncs, id, sync)
    }
}
