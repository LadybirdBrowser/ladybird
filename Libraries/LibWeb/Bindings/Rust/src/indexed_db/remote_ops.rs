/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The operations that requests hand the backend, and the conversions that turn what the backend found into the
//! requests' results, in the requests' realms.

use std::rc::Rc;

use ak::Utf16String;

use libjs_rust::api::{Array, Gc, Must, PropertyKey, Realm, Value, Vm};
use libwebcommon_rust::indexed_db::Name;
use libwebcommon_rust::indexed_db::backend::cursor::{Direction, FoundRecord, Source};
use libwebcommon_rust::indexed_db::backend::store::{GENERATED_KEY_PLACEHOLDER, IndexKeys, NamedIndexKeys};
use libwebcommon_rust::indexed_db::backend::{IndexKeyEntry, OperationResult, Retrieve};
use libwebcommon_rust::indexed_db::key_range::KeyRange;

use crate::bindings::CallContext;
use crate::bindings::WebIdlResult;
use crate::bindings::conversions::ToJs;
use crate::bindings::interface::with_temporary_execution_context_and_callbacks;
use crate::indexed_db::idb_cursor::{AnyIDBCursor, IDBCursorDirection};
use crate::indexed_db::idb_key_range::IDBKeyRange;
use crate::indexed_db::idb_object_store::idb_cursor_result;
use crate::indexed_db::idb_record::{IDBRecord, StorageSerializationRecord};
use crate::indexed_db::internal::algorithms::{
    KeyPath, RecordKind, RecordSource, check_that_a_key_could_be_injected_into_a_value, convert_a_key_to_a_value,
    deserialize_a_stored_record, extract_a_key_from_a_value_using_a_key_path,
    inject_a_key_into_a_value_using_a_key_path,
};
use crate::indexed_db::internal::function::{Conversion, NoCells, create_conversion};
use crate::indexed_db::internal::index::IndexRef;
use crate::indexed_db::internal::key::{Key, KeyType};
use crate::indexed_db::internal::object_store::ObjectStoreRef;
use crate::indexed_db::remote::name_from;

pub(crate) fn backend_range(range: &IDBKeyRange) -> KeyRange {
    KeyRange {
        lower: range.lower_key().map(|key| key.as_ref().clone()),
        upper: range.upper_key().map(|key| key.as_ref().clone()),
        lower_open: range.is_lower_open(),
        upper_open: range.is_upper_open(),
    }
}

pub(crate) fn store_source(store: ObjectStoreRef) -> Source {
    Source::ObjectStore(name_from(&store.name()))
}

pub(crate) fn index_source(index: IndexRef) -> Source {
    Source::Index {
        store: name_from(&index.object_store().name()),
        index: name_from(&index.name()),
    }
}

pub(crate) fn backend_direction(direction: IDBCursorDirection) -> Direction {
    match direction {
        IDBCursorDirection::Next => Direction::Next,
        IDBCursorDirection::Nextunique => Direction::NextUnique,
        IDBCursorDirection::Prev => Direction::Prev,
        IDBCursorDirection::Prevunique => Direction::PrevUnique,
    }
}

pub(crate) fn backend_retrieve(kind: RecordKind) -> Retrieve {
    match kind {
        RecordKind::Key => Retrieve::Keys,
        RecordKind::Value => Retrieve::Values,
        RecordKind::Record => Retrieve::Records,
    }
}

/// The keys under which a value goes into each index of its object store, https://w3c.github.io/IndexedDB/#store-a-record-into-an-object-store
/// step 5.1: the result of extracting a key from the value using the index's key path, and none for an index where
/// that is an exception, invalid, or failure.
pub(crate) fn index_keys_of(
    cx: &CallContext,
    store: ObjectStoreRef,
    value: Value,
    key_is_generated: bool,
) -> Vec<NamedIndexKeys> {
    let vm = cx.vm();

    // NB: The backend generates the key, and only then is it injected into the value, so an index key that the
    //     store's key path finds gets the placeholder, which the backend replaces with the key. The value was already
    //     serialized, so the placeholder doesn't end up in the record.
    // AD-HOC: The spec injects the key before it extracts the index keys, so another key path reaches the key too
    //         where the value holds one object at both. For web compatibility, such an index key is extracted as if
    //         the key weren't there, so the record has none. Injecting a second, different placeholder shows which
    //         index keys the injection reached.
    let injected_at = if key_is_generated
        && store.uses_inline_keys()
        && let Some(key_path) = store.key_path()
        && check_that_a_key_could_be_injected_into_a_value(cx, value, key_path)
    {
        Some(key_path)
    } else {
        None
    };
    let inject = |bytes: &[u8]| {
        if let Some(key_path) = injected_at {
            inject_a_key_into_a_value_using_a_key_path(cx, value, &Key::create_binary(bytes.to_vec()), key_path);
        }
    };
    let extract_all = || -> Vec<(Name, Vec<Key>)> {
        store
            .indexes(vm)
            .values()
            .iter()
            .map(|index| {
                let keys =
                    match extract_a_key_from_a_value_using_a_key_path(cx, value, index.key_path(), index.multi_entry())
                    {
                        // 6. If index's multiEntry flag is false, or if index key is not an array key, then store a
                        //    record in index containing index key as its key.
                        // 7. If index's multiEntry flag is true and index key is an array key, then for each subkey of
                        //    the subkeys of index key store a record in index containing subkey as its key.
                        Ok(Ok(key)) if !key.is_invalid() => {
                            if index.multi_entry() && key.r#type() == KeyType::Array {
                                key.subkeys().iter().map(|subkey| subkey.as_ref().clone()).collect()
                            } else {
                                vec![key.as_ref().clone()]
                            }
                        }
                        _ => Vec::new(),
                    };
                (name_from(&index.name()), keys)
            })
            .collect()
    };

    inject(GENERATED_KEY_PLACEHOLDER);
    let entries = extract_all();
    let probes = match injected_at {
        Some(_) => {
            inject(OTHER_GENERATED_KEY_PLACEHOLDER);
            extract_all()
        }
        None => Vec::new(),
    };
    let store_key_path = match injected_at {
        Some(KeyPath::String(path)) => Some(path),
        _ => None,
    };
    let indexes = store.indexes(vm);
    entries
        .into_iter()
        .enumerate()
        .map(|(position, (index, keys))| {
            let reached_elsewhere = probes.get(position).is_some_and(|(_, probe_keys)| {
                injection_reached_elsewhere(indexes.values()[position].key_path(), store_key_path, &keys, probe_keys)
            });
            NamedIndexKeys {
                index,
                keys: IndexKeys::Keys(if reached_elsewhere { Vec::new() } else { keys }),
            }
        })
        .collect()
}

/// A second key to inject in place of the generated one, which shows where an index key holds the injected key.
const OTHER_GENERATED_KEY_PLACEHOLDER: &[u8] = b"\xff\xfe\x00Ladybird generated key probe\x00\xfe\xff";

/// Whether the injected placeholder reached an index's keys anywhere but at the store's key path: where the keys
/// extracted with the other placeholder injected instead differ, which also catches a key that a key path derived from
/// the placeholder (its byteLength, e.g.).
fn injection_reached_elsewhere(
    index_key_path: &KeyPath,
    store_key_path: Option<&Utf16String>,
    keys: &[Key],
    probe_keys: &[Key],
) -> bool {
    if keys.len() != probe_keys.len() {
        return true;
    }
    match index_key_path {
        KeyPath::String(path) => {
            Some(path) != store_key_path && keys.iter().zip(probe_keys).any(|(key, probe)| !Key::equals(key, probe))
        }
        KeyPath::Sequence(paths) => keys.iter().zip(probe_keys).any(|(key, probe)| {
            if key.r#type() != KeyType::Array
                || probe.r#type() != KeyType::Array
                || key.subkeys().len() != probe.subkeys().len()
            {
                return !Key::equals(key, probe);
            }
            key.subkeys()
                .iter()
                .zip(probe.subkeys())
                .zip(paths)
                .any(|((subkey, probe_subkey), path)| {
                    Some(path) != store_key_path && !Key::equals(subkey, probe_subkey)
                })
        }),
    }
}

/// The value of a record the backend found, deserialized in the agent's realm. The backend generated the key of a
/// record of an object store with a key generator and in-line keys, and the value gets that key here, which only the
/// agent can inject.
pub(crate) fn read_record_value(cx: &CallContext, store: ObjectStoreRef, record: &FoundRecord) -> WebIdlResult<Value> {
    let serialized: StorageSerializationRecord = Rc::from(record.value.as_deref().unwrap_or_default());
    let value = deserialize_a_stored_record(cx, &serialized)?;
    if store.uses_inline_keys()
        && store.uses_a_key_generator()
        && let Some(key_path) = store.key_path()
    {
        inject_a_generated_key(cx, value, key_path, &record.primary_key);
    }
    Ok(value)
}

/// Gives the value of a record of an object store with a key generator and in-line keys the key the backend generated
/// for it, unless it has a key at the key path already, which means it wasn't given a generated one.
pub(crate) fn inject_a_generated_key(cx: &CallContext, value: Value, key_path: &KeyPath, primary_key: &Key) {
    let has_key = matches!(
        extract_a_key_from_a_value_using_a_key_path(cx, value, key_path, false),
        Ok(Ok(key)) if !key.is_invalid()
    );
    if !has_key && check_that_a_key_could_be_injected_into_a_value(cx, value, key_path) {
        inject_a_key_into_a_value_using_a_key_path(cx, value, &Rc::new(primary_key.clone()), key_path);
    }
}

fn in_realm<R>(vm: &Vm, realm: Gc<Realm>, steps: impl FnOnce(&CallContext) -> R) -> R {
    with_temporary_execution_context_and_callbacks(realm, || steps(&CallContext::new(vm, realm)))
}

pub(crate) fn undefined_conversion(vm: &Vm) -> Conversion {
    create_conversion(vm, (), |_, (), _| Ok(Value::UNDEFINED))
}

pub(crate) fn count_conversion(vm: &Vm) -> Conversion {
    create_conversion(vm, (), |_, (), result| match result {
        #[allow(clippy::cast_precision_loss, reason = "a count of records fits a double")]
        OperationResult::Count(count) => Ok(Value::from_f64(count as f64)),
        _ => Ok(Value::UNDEFINED),
    })
}

/// A key, or undefined for none, as a value.
pub(crate) fn key_conversion(vm: &Vm, realm: Gc<Realm>) -> Conversion {
    create_conversion(vm, realm, |vm, &realm, result| {
        let key = match result {
            OperationResult::Key(key) => Some(key),
            OperationResult::OptionalKey(key) => key,
            _ => None,
        };
        Ok(match key {
            Some(key) => in_realm(vm, realm, |cx| convert_a_key_to_a_value(cx, &Rc::new(key))),
            None => Value::UNDEFINED,
        })
    })
}

/// The value of the record that the backend found, or undefined for none.
pub(crate) fn value_conversion(vm: &Vm, realm: Gc<Realm>, store: ObjectStoreRef) -> Conversion {
    create_conversion(vm, (realm, store), |vm, &(realm, store), result| match result {
        OperationResult::Value(Some(record)) => in_realm(vm, realm, |cx| read_record_value(cx, store, &record)),
        _ => Ok(Value::UNDEFINED),
    })
}

/// https://w3c.github.io/IndexedDB/#retrieve-multiple-items-from-an-object-store step 6, and the same for an index:
/// the list of keys, values or record snapshots of the records that the backend found.
pub(crate) fn records_conversion(
    vm: &Vm,
    realm: Gc<Realm>,
    store: ObjectStoreRef,
    kind: RecordKind,
    from_index: bool,
) -> Conversion {
    create_conversion(
        vm,
        (realm, store, NoCells((kind, from_index))),
        |vm, &(realm, store, NoCells((kind, from_index))), result| {
            let OperationResult::Records(records) = result else {
                return Ok(Value::UNDEFINED);
            };
            in_realm(vm, realm, |cx| {
                let list = Array::create(vm, realm, records.len() as u64, None).must();
                for (position, record) in records.iter().enumerate() {
                    let property_key = PropertyKey::from_number(position as u64);
                    let entry = match kind {
                        // The key of an object store's record, or the key of the object store record that an index's
                        // record references.
                        RecordKind::Key => {
                            let key = if from_index { &record.primary_key } else { &record.key };
                            convert_a_key_to_a_value(cx, &Rc::new(key.clone()))
                        }
                        RecordKind::Value => read_record_value(cx, store, record)?,
                        RecordKind::Record => {
                            let value = read_record_value(cx, store, record)?;
                            IDBRecord::create(
                                vm,
                                Rc::new(record.key.clone()),
                                value,
                                Rc::new(record.primary_key.clone()),
                            )
                            .to_js(cx)?
                        }
                    };
                    list.create_data_property_or_throw(vm, &property_key, entry).must();
                }
                Ok(Value::from_object(list))
            })
        },
    )
}

/// The key that an operation is handed, as the backend takes it.
pub(crate) fn backend_key(key: &Rc<Key>) -> Key {
    key.as_ref().clone()
}

/// https://w3c.github.io/IndexedDB/#iterate-a-cursor steps 9.2 and 10 to 15, for the record that the backend's cursor
/// found: the agent's cursor takes on its position, key and value, or reports that it found none.
pub(crate) fn cursor_conversion(vm: &Vm, realm: Gc<Realm>, cursor: AnyIDBCursor) -> Conversion {
    create_conversion(vm, (realm, cursor), |vm, &(realm, cursor), result| {
        let OperationResult::Cursor(found) = result else {
            return Ok(Value::UNDEFINED);
        };
        let source_is_index = matches!(cursor.internal_source(), RecordSource::Index(_));
        in_realm(vm, realm, |cx| {
            // 9.2. If found record is not defined, then:
            let Some(record) = found else {
                // 1. Set cursor's key to undefined.
                cursor.set_key(None);

                // 2. If source is an index, set cursor's object store position to undefined.
                if source_is_index {
                    cursor.set_object_store_position(None);
                }

                // 3. If cursor's key only flag is false, set cursor's value to undefined.
                if !cursor.key_only() {
                    cursor.set_value(Some(Value::UNDEFINED));
                }

                // 4. Return null.
                return Ok(idb_cursor_result(cx, None));
            };
            let key = Rc::new(record.key.clone());

            // 10. Set cursor's position to position.
            cursor.set_position(Some(key.clone()));

            // 11. If source is an index, set cursor's object store position to object store position.
            if source_is_index {
                cursor.set_object_store_position(Some(Rc::new(record.primary_key.clone())));
            }

            // 12. Set cursor's key to found record's key.
            cursor.set_key(Some(key));

            // 13. If cursor's key only flag is false, then set cursor's value to the deserialized value.
            if !cursor.key_only() {
                cursor.set_value(Some(read_record_value(cx, cursor.effective_object_store(), &record)?));
            }

            // 14. Set cursor's got value flag to true.
            cursor.set_got_value(true);

            // 15. Return cursor.
            Ok(idb_cursor_result(cx, Some(cursor)))
        })
    })
}

/// The index records of an existing object store record, for an index that an upgrade transaction created.
pub(crate) struct IndexKeyEntryOf;

impl IndexKeyEntryOf {
    pub(crate) fn entry(cx: &CallContext, index: IndexRef, value: Value, primary_key: &Key) -> IndexKeyEntry {
        let keys = match extract_a_key_from_a_value_using_a_key_path(cx, value, index.key_path(), index.multi_entry()) {
            Ok(Ok(key)) if !key.is_invalid() => {
                if index.multi_entry() && key.r#type() == KeyType::Array {
                    key.subkeys().iter().map(|subkey| subkey.as_ref().clone()).collect()
                } else {
                    vec![key.as_ref().clone()]
                }
            }
            _ => Vec::new(),
        };
        IndexKeyEntry {
            primary_key: primary_key.clone(),
            keys,
        }
    }
}
