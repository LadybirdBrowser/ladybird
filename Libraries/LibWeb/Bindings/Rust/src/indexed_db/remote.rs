/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The agent's half of the algorithms that the backend in Hub runs the rest of. The backend holds every database of the
//! browsing session, and decides when a connection opens, when a transaction starts, and what an operation finds; the
//! agent clones values, extracts keys from them, and fires the events, as the backend's events tell it to.

use core::cell::Cell;
use core::ffi::c_void;
use std::collections::HashMap;
use std::hash::Hash;

use ak::Utf16String;
use libjs_rust::api::{EmbedderCell, Gc, GcRefCell, GcWeak, MarkedVec, Realm, Trace, Visitor, Vm};
use libwebcommon_rust::indexed_db::Name;
use libwebcommon_rust::indexed_db::backend::store::KeyPath as BackendKeyPath;
use libwebcommon_rust::indexed_db::backend::{
    DatabaseInfo, DatabaseMetadata, Error as BackendError, ErrorKind, Event, Operation as BackendOperation,
    OperationResult, Request,
};

use crate::bindings::environment_settings_object::relevant_settings_object;
use crate::bindings::global_object::window_from_global_object;
use crate::bindings::interface::{Ref, Wrapped};
use crate::indexed_db::client;
use crate::indexed_db::cpp::DomException;
use crate::indexed_db::idb_database::IDBDatabase;
use crate::indexed_db::idb_request::AnyIDBRequest;
use crate::indexed_db::idb_transaction::IDBTransaction;
use crate::indexed_db::internal::algorithms::{KeyPath, fire_a_version_change_event, queue_a_database_task};
use crate::indexed_db::internal::database::{Database, DatabaseRef};
use crate::indexed_db::internal::function::{Function, FunctionWith, NoCells, create_function};
use crate::indexed_db::internal::index::Index;
use crate::indexed_db::internal::object_store::ObjectStore;
use crate::indexed_db::internal::storage::storage;

/// A map whose values are cells or hold them.
pub(crate) struct CellMap<K, V>(HashMap<K, V>);

impl<K, V> Default for CellMap<K, V> {
    fn default() -> Self {
        Self(HashMap::new())
    }
}

// SAFETY: The values are all the cells the map reaches.
unsafe impl<K, V: Trace> Trace for CellMap<K, V> {
    fn trace<'c>(&'c self, visitor: &mut Visitor<'c>) {
        for value in self.0.values() {
            value.trace(visitor);
        }
    }
}

impl<K: Eq + Hash, V> CellMap<K, V> {
    fn insert(&mut self, key: K, value: V) {
        self.0.insert(key, value);
    }

    fn remove(&mut self, key: &K) -> Option<V> {
        self.0.remove(key)
    }

    fn get(&self, key: &K) -> Option<&V> {
        self.0.get(key)
    }
}

pub(crate) use crate::indexed_db::internal::algorithms::{DeleteResult, OpenResult};

/// What an operation's result is converted by, in the agent's realm, once it arrives.
pub(crate) type ResultHandler = FunctionWith<Result<OperationResult, BackendError>>;

#[derive(Trace)]
struct PendingOpen {
    request: AnyIDBRequest,
    realm: Gc<Realm>,
    on_complete: FunctionWith<OpenResult>,
    // Once the backend asks for an upgrade, the upgrade transaction decides the outcome, as the agent runs it.
    upgrading: Cell<bool>,
}

#[derive(Trace)]
struct PendingDelete {
    request: AnyIDBRequest,
    realm: Gc<Realm>,
    on_complete: FunctionWith<DeleteResult>,
}

/// What runs with the databases that a databases() call listed.
type DatabaseListHandler = FunctionWith<Result<Vec<DatabaseInfo>, BackendError>>;

/// The handler of an operation's result, with the transaction the operation is for.
type PendingResult = (NoCells<u64>, ResultHandler);

type WeakConnection = GcWeak<EmbedderCell<Wrapped<IDBDatabase>>>;

/// The agent's clients, by the realm whose environment each was connected for, and everything that awaits an event.
#[derive(Trace, Default)]
pub(crate) struct Remote {
    clients: GcRefCell<Vec<(Gc<Realm>, NoCells<u64>)>>,
    opens: GcRefCell<CellMap<(u64, u64), PendingOpen>>,
    deletes: GcRefCell<CellMap<(u64, u64), PendingDelete>>,
    database_lists: GcRefCell<CellMap<(u64, u64), DatabaseListHandler>>,
    // NB: Held weakly, so a connection that script can no longer reach is collected, which closes it.
    connections: GcRefCell<CellMap<(u64, u64), WeakConnection>>,
    transactions: GcRefCell<CellMap<(u64, u64), Ref<IDBTransaction>>>,
    // The handlers of the operations that the backend hasn't answered yet, with their transactions.
    results: GcRefCell<CellMap<(u64, u64), PendingResult>>,
    commits: GcRefCell<CellMap<(u64, u64), Function>>,
    // The realms whose environments unloaded, whose clients go once nothing they started is left to finish.
    unloaded: GcRefCell<Vec<Gc<Realm>>>,
    #[gc(untraced)]
    next_id: Cell<u64>,
}

fn remote(vm: &Vm) -> crate::indexed_db::internal::storage::StorageRef {
    storage(vm)
}

/// A number for a request, transaction or cursor that no other one of this agent has.
pub(crate) fn next_id(vm: &Vm) -> u64 {
    let storage = remote(vm);
    let remote = storage.remote();
    let id = remote.next_id.get() + 1;
    remote.next_id.set(id);
    id
}

/// The client of a realm's environment, connected on first use. None when the agent can't reach the backend.
pub(crate) fn client_for_realm(vm: &Vm, realm: Gc<Realm>, page_id: u64, environment: *mut c_void) -> Option<u64> {
    let storage = remote(vm);
    if let Some((_, id)) = storage
        .remote()
        .clients
        .borrow()
        .iter()
        .find(|(entry, _)| *entry == realm)
    {
        return Some(id.0);
    }
    let client_id = client::connect(page_id, environment)?;
    storage
        .remote()
        .clients
        .borrow_mut(vm)
        .push((realm, NoCells(client_id)));
    Some(client_id)
}

/// The client of a realm's environment: a window's or a worker's.
pub(crate) fn client_for(vm: &Vm, realm: Gc<Realm>) -> Option<u64> {
    let global = realm.global_object();
    let environment = relevant_settings_object(global);
    let page_id = window_from_global_object(global).map_or(0, |window| window.page_id());
    client_for_realm(vm, realm, page_id, environment.as_ptr())
}

/// Forgets a realm's client once its environment goes away, after its connections were closed.
fn forget_realm(vm: &Vm, realm: Gc<Realm>) {
    let storage = remote(vm);
    let mut clients = storage.remote().clients.borrow_mut(vm);
    if let Some(position) = clients.iter().position(|(entry, _)| *entry == realm) {
        let (_, id) = clients.remove(position);
        drop(clients);
        client::disconnect(id.0);
    }
}

/// Disconnects the client of a realm whose environment unloaded, once its connections' transactions, and its open and
/// delete requests, are done: Disconnecting aborts whatever the backend still runs for the client.
pub(crate) fn forget_realm_when_idle(vm: &Vm, realm: Gc<Realm>) {
    {
        let storage = remote(vm);
        let mut unloaded = storage.remote().unloaded.borrow_mut(vm);
        if !unloaded.contains(&realm) {
            unloaded.push(realm);
        }
    }
    forget_idle_realms(vm);
}

/// Forgets each unloaded realm that has nothing left to finish.
pub(crate) fn forget_idle_realms(vm: &Vm) {
    let storage = remote(vm);
    let remote = storage.remote();
    let realms = MarkedVec::new(vm);
    realms.extend_from_slice(&remote.unloaded.borrow());
    for realm in realms.values().iter().copied() {
        let client = remote
            .clients
            .borrow()
            .iter()
            .find(|(entry, _)| *entry == realm)
            .map(|(_, id)| id.0);
        let busy = client.is_some_and(|client| {
            let of_client = |key: &(u64, u64)| key.0 == client;
            remote.opens.borrow().0.keys().any(of_client)
                || remote.deletes.borrow().0.keys().any(of_client)
                || remote.database_lists.borrow().0.keys().any(of_client)
                || remote.transactions.borrow().0.keys().any(of_client)
                || remote.results.borrow().0.keys().any(of_client)
                || remote.commits.borrow().0.keys().any(of_client)
        });
        if busy {
            continue;
        }
        remote.unloaded.borrow_mut(vm).retain(|entry| *entry != realm);
        forget_realm(vm, realm);
    }
}

pub(crate) fn send(client_id: u64, request: Request) {
    // NB: The backend hears of collected connections before anything that might wait on them.
    close_collected_connections();
    client::send(client_id, &request);
}

/// Tells the backend to close the connections that the garbage collector finalized.
pub(crate) fn close_collected_connections() {
    for (client, connection) in COLLECTED_CONNECTIONS.with(|collected| core::mem::take(&mut *collected.borrow_mut())) {
        client::send(client, &Request::Close { connection });
    }
}

thread_local! {
    // The connections that the garbage collector finalized while they were open, by client and the backend's number,
    // which a task after the collection closes, or the next request if it comes first: A finalizer can't send one.
    static COLLECTED_CONNECTIONS: core::cell::RefCell<Vec<(u64, u64)>> = const { core::cell::RefCell::new(Vec::new()) };
}

/// Closes a connection that the garbage collector finalized, once the collection is over.
pub(crate) fn connection_collected(client_id: u64, connection_id: u64) {
    let first = COLLECTED_CONNECTIONS.with(|collected| {
        let mut collected = collected.borrow_mut();
        collected.push((client_id, connection_id));
        collected.len() == 1
    });
    if first {
        crate::indexed_db::cpp::enqueue_close_collected_connections();
    }
}

/// A request that a transaction holds back, or the point where the next population of an index starts holding them.
enum Held {
    Request(Request),
    Barrier,
}

thread_local! {
    // The requests of transactions that hold their requests back, until the agent sent what must reach the backend
    // first: the index records of a new index, before any request made after the index was created.
    static HELD: core::cell::RefCell<HashMap<(u64, u64), Vec<Held>>> = core::cell::RefCell::new(HashMap::new());
}

/// Sends a request of a transaction, unless the transaction holds its requests back.
fn send_for_transaction(client_id: u64, transaction_id: u64, request: Request) {
    let request = HELD.with_borrow_mut(|held| match held.get_mut(&(client_id, transaction_id)) {
        Some(entries) => {
            entries.push(Held::Request(request));
            None
        }
        None => Some(request),
    });
    if let Some(request) = request {
        send(client_id, request);
    }
}

/// Holds back the requests that a transaction makes from now on, until a release.
pub(crate) fn hold(transaction: Ref<IDBTransaction>) {
    HELD.with_borrow_mut(
        |held| match held.get_mut(&(transaction.remote_client(), transaction.remote_id())) {
            Some(entries) => entries.push(Held::Barrier),
            None => {
                held.insert((transaction.remote_client(), transaction.remote_id()), Vec::new());
            }
        },
    );
}

/// Sends what a transaction held back since the hold that this releases, up to the next hold.
pub(crate) fn release(transaction: Ref<IDBTransaction>) {
    let key = (transaction.remote_client(), transaction.remote_id());
    let requests = HELD.with_borrow_mut(|held| {
        let entries = held.get_mut(&key)?;
        let barrier = entries.iter().position(|entry| matches!(entry, Held::Barrier));
        let ready: Vec<Held> = match barrier {
            Some(position) => {
                let ready = entries.drain(..position).collect();
                // The next hold becomes the active one.
                entries.remove(0);
                ready
            }
            None => held.remove(&key).unwrap_or_default(),
        };
        Some(ready)
    });
    for entry in requests.unwrap_or_default() {
        if let Held::Request(request) = entry {
            send(key.0, request);
        }
    }
}

/// Sends a request of a transaction now, ahead of the ones it holds back.
pub(crate) fn perform_now(
    vm: &Vm,
    transaction: Ref<IDBTransaction>,
    operation: BackendOperation,
    handler: ResultHandler,
) {
    let client_id = transaction.remote_client();
    let request_id = next_id(vm);
    remote(vm)
        .remote()
        .results
        .borrow_mut(vm)
        .insert((client_id, request_id), (NoCells(transaction.remote_id()), handler));
    send(
        client_id,
        Request::Operation {
            transaction: transaction.remote_id(),
            request: request_id,
            operation,
        },
    );
}

pub(crate) fn dom_exception_from(error: &BackendError) -> DomException {
    let name = match error.kind {
        ErrorKind::Abort => "AbortError",
        ErrorKind::Constraint => "ConstraintError",
        ErrorKind::Data => "DataError",
        ErrorKind::InvalidState => "InvalidStateError",
        ErrorKind::NotFound => "NotFoundError",
        ErrorKind::TransactionInactive => "TransactionInactiveError",
        ErrorKind::Unknown => "UnknownError",
        ErrorKind::Version => "VersionError",
        ErrorKind::QuotaExceeded => "QuotaExceededError",
    };
    DomException::create(name, &error.message)
}

pub(crate) fn name_from(string: &Utf16String) -> Name {
    libipc_rust::wire::Utf16String(string.to_utf16().into_owned())
}

pub(crate) fn string_from(name: &Name) -> Utf16String {
    Utf16String::from_utf16(&name.0)
}

pub(crate) fn backend_key_path(key_path: &KeyPath) -> BackendKeyPath {
    match key_path {
        KeyPath::String(path) => BackendKeyPath::String(name_from(path)),
        KeyPath::Sequence(paths) => BackendKeyPath::Sequence(paths.iter().map(name_from).collect()),
    }
}

pub(crate) fn key_path_from(key_path: &BackendKeyPath) -> KeyPath {
    match key_path {
        BackendKeyPath::String(path) => KeyPath::String(string_from(path)),
        BackendKeyPath::Sequence(paths) => KeyPath::Sequence(paths.iter().map(string_from).collect()),
    }
}

/// A database as a connection sees it: the backend's metadata, with no records, which only the backend holds.
fn database_from_metadata(vm: &Vm, metadata: &DatabaseMetadata) -> DatabaseRef {
    let database = Database::create(vm, &string_from(&metadata.name));
    database.set_version(metadata.version);
    for store in &metadata.object_stores {
        // NB: A new object store adds itself to its database, and a new index itself to its object store.
        let object_store = ObjectStore::create(
            vm,
            database,
            string_from(&store.name),
            store.auto_increment,
            store.key_path.as_ref().map(key_path_from),
        );
        for index in &store.indexes {
            Index::create(
                vm,
                object_store,
                &string_from(&index.name),
                &key_path_from(&index.key_path),
                index.unique,
                index.multi_entry,
            );
        }
    }
    database
}

// https://w3c.github.io/IndexedDB/#open-a-database-connection
pub(crate) fn open_a_database_connection(
    vm: &Vm,
    realm: Gc<Realm>,
    client_id: u64,
    name: &Utf16String,
    version: Option<u64>,
    request: AnyIDBRequest,
    on_complete: FunctionWith<OpenResult>,
) {
    let request_id = next_id(vm);
    remote(vm).remote().opens.borrow_mut(vm).insert(
        (client_id, request_id),
        PendingOpen {
            request,
            realm,
            on_complete,
            upgrading: Cell::new(false),
        },
    );
    send(
        client_id,
        Request::Open {
            request: request_id,
            name: name_from(name),
            version,
        },
    );
}

// https://w3c.github.io/IndexedDB/#delete-a-database
pub(crate) fn delete_a_database(
    vm: &Vm,
    realm: Gc<Realm>,
    client_id: u64,
    name: &Utf16String,
    request: AnyIDBRequest,
    on_complete: FunctionWith<DeleteResult>,
) {
    let request_id = next_id(vm);
    remote(vm).remote().deletes.borrow_mut(vm).insert(
        (client_id, request_id),
        PendingDelete {
            request,
            realm,
            on_complete,
        },
    );
    send(
        client_id,
        Request::DeleteDatabase {
            request: request_id,
            name: name_from(name),
        },
    );
}

// https://w3c.github.io/IndexedDB/#dom-idbfactory-databases
pub(crate) fn list_databases(vm: &Vm, client_id: u64, on_complete: DatabaseListHandler) {
    let request_id = next_id(vm);
    remote(vm)
        .remote()
        .database_lists
        .borrow_mut(vm)
        .insert((client_id, request_id), on_complete);
    send(client_id, Request::Databases { request: request_id });
}

pub(crate) fn register_transaction(vm: &Vm, transaction: Ref<IDBTransaction>) {
    let key = (transaction.remote_client(), transaction.remote_id());
    remote(vm).remote().transactions.borrow_mut(vm).insert(key, transaction);
}

/// Hands the backend an operation of a transaction, and the handler of its result.
pub(crate) fn perform(vm: &Vm, transaction: Ref<IDBTransaction>, operation: BackendOperation, handler: ResultHandler) {
    let client_id = transaction.remote_client();
    let request_id = next_id(vm);
    remote(vm)
        .remote()
        .results
        .borrow_mut(vm)
        .insert((client_id, request_id), (NoCells(transaction.remote_id()), handler));
    send_for_transaction(
        client_id,
        transaction.remote_id(),
        Request::Operation {
            transaction: transaction.remote_id(),
            request: request_id,
            operation,
        },
    );
}

/// Asks the backend to commit a transaction, and runs the steps once it has.
pub(crate) fn commit(vm: &Vm, transaction: Ref<IDBTransaction>, after: Function) {
    let key = (transaction.remote_client(), transaction.remote_id());
    remote(vm).remote().commits.borrow_mut(vm).insert(key, after);
    send_for_transaction(key.0, key.1, Request::Commit { transaction: key.1 });
}

pub(crate) fn abort(vm: &Vm, transaction: Ref<IDBTransaction>) {
    let key = (transaction.remote_client(), transaction.remote_id());
    let storage = remote(vm);
    storage.remote().commits.borrow_mut(vm).remove(&key);
    storage.remote().transactions.borrow_mut(vm).remove(&key);
    forget_results_of(vm, key);
    HELD.with_borrow_mut(|held| held.remove(&key));
    send(key.0, Request::Abort { transaction: key.1 });
}

/// Drops the handlers of a transaction's operations, which the backend never answers once the transaction aborted.
fn forget_results_of(vm: &Vm, (client_id, transaction): (u64, u64)) {
    remote(vm)
        .remote()
        .results
        .borrow_mut(vm)
        .0
        .retain(|(client, _), (owner, _)| *client != client_id || owner.0 != transaction);
}

pub(crate) fn close(vm: &Vm, connection: Ref<IDBDatabase>) {
    let key = (connection.remote_client(), connection.remote_id());
    remote(vm).remote().connections.borrow_mut(vm).remove(&key);
    send(key.0, Request::Close { connection: key.1 });
}

/// The connections of a realm that are not closing, for when its environment goes away.
pub(crate) fn connections_of_realm(vm: &Vm, realm: Gc<Realm>) -> MarkedVec<'_, Ref<IDBDatabase>> {
    let connections = MarkedVec::new(vm);
    for connection in remote(vm)
        .remote()
        .connections
        .borrow()
        .0
        .values()
        .filter_map(GcWeak::get)
    {
        if connection.realm() == realm {
            connections.push(connection);
        }
    }
    connections
}

fn create_connection(
    vm: &Vm,
    realm: Gc<Realm>,
    client_id: u64,
    connection_id: u64,
    metadata: &DatabaseMetadata,
) -> Ref<IDBDatabase> {
    // NB: The entries of collected connections go too.
    remote(vm)
        .remote()
        .connections
        .borrow_mut(vm)
        .0
        .retain(|_, connection| connection.get().is_some());
    let database = database_from_metadata(vm, metadata);
    let connection = IDBDatabase::create(vm, realm, database);
    connection.set_version(metadata.version);
    connection.set_remote(client_id, connection_id);
    remote(vm)
        .remote()
        .connections
        .borrow_mut(vm)
        .insert((client_id, connection_id), GcWeak::new(vm.heap(), connection));
    connection
}

pub(crate) fn handle_event(vm: &Vm, client_id: u64, event: Event) {
    let storage = remote(vm);
    let remote = storage.remote();
    match event {
        Event::UpgradeNeeded {
            request,
            connection,
            transaction,
            old_version,
            metadata,
        } => {
            // NB: An upgrade for a document that unloaded doesn't run: Nothing would run its upgradeneeded listener,
            //     so the upgrade would commit without the changes it was for. The open fails instead, and the
            //     connection queue goes on once the backend aborted the upgrade.
            let unloaded = remote
                .opens
                .borrow()
                .get(&(client_id, request))
                .is_some_and(|pending| remote.unloaded.borrow().contains(&pending.realm));
            if unloaded {
                remote.opens.borrow_mut(vm).remove(&(client_id, request));
                send(client_id, Request::Abort { transaction });
                send(client_id, Request::OpenHandled { request });
                return;
            }
            let Some(pending) = remote.opens.borrow().get(&(client_id, request)).map(|pending| {
                pending.upgrading.set(true);
                (pending.realm, pending.request, pending.on_complete)
            }) else {
                return;
            };
            let (realm, open_request, on_complete) = pending;
            let mut old_metadata = metadata.clone();
            old_metadata.version = old_version;
            let connection = create_connection(vm, realm, client_id, connection, &old_metadata);

            // https://w3c.github.io/IndexedDB/#open-a-database-connection
            // 9. Set connection's version to version.
            connection.set_version(metadata.version);
            // NB: Once the upgrade's outcome is queued, the connection queue's next request may go on.
            let on_complete = crate::indexed_db::internal::function::create_function_with(
                vm,
                (on_complete, NoCells((client_id, request))),
                |vm, &(on_complete, NoCells((client_id, request))), result: OpenResult| {
                    on_complete.call(vm, result);
                    send(client_id, Request::OpenHandled { request });
                },
            );
            crate::indexed_db::internal::algorithms::run_upgrade_for_backend(
                vm,
                realm,
                connection,
                metadata.version,
                transaction,
                open_request,
                on_complete,
            );
        }
        Event::OpenSucceeded {
            request,
            connection,
            metadata,
        } => {
            let Some(pending) = remote.opens.borrow_mut(vm).remove(&(client_id, request)) else {
                return;
            };
            // NB: After an upgrade, the upgrade transaction already completed the open request.
            if pending.upgrading.get() {
                return;
            }
            // NB: A document that unloaded gets no connection: Nothing would close it, and it would keep the
            //     document's realm alive.
            if remote.unloaded.borrow().contains(&pending.realm) {
                send(client_id, Request::Close { connection });
                send(client_id, Request::OpenHandled { request });
                return;
            }
            let connection = create_connection(vm, pending.realm, client_id, connection, &metadata);
            pending.on_complete.call(vm, Ok(connection));

            // NB: The success event is queued, so the connection queue's next request may go on.
            send(client_id, Request::OpenHandled { request });
        }
        Event::OpenFailed { request, error } => {
            let Some(pending) = remote.opens.borrow_mut(vm).remove(&(client_id, request)) else {
                return;
            };
            if pending.upgrading.get() {
                return;
            }
            pending.on_complete.call(vm, Err(dom_exception_from(&error)));
        }
        Event::Blocked {
            request,
            old_version,
            new_version,
        } => {
            let target = remote
                .opens
                .borrow()
                .get(&(client_id, request))
                .map(|pending| (pending.realm, pending.request))
                .or_else(|| {
                    remote
                        .deletes
                        .borrow()
                        .get(&(client_id, request))
                        .map(|pending| (pending.realm, pending.request))
                });
            let Some((realm, request)) = target else {
                return;
            };
            queue_a_database_task(
                vm,
                create_function(
                    vm,
                    (realm, request, NoCells((old_version, new_version))),
                    |vm, &(realm, request, NoCells((old_version, new_version))), ()| {
                        fire_a_version_change_event(
                            vm,
                            realm,
                            "blocked",
                            request.event_target(),
                            old_version,
                            new_version,
                        );
                    },
                ),
            );
        }
        Event::VersionChange {
            connection,
            old_version,
            new_version,
        } => {
            let entry = remote
                .connections
                .borrow()
                .get(&(client_id, connection))
                .and_then(GcWeak::get);
            let Some(entry) = entry else {
                send(client_id, Request::VersionChangeHandled { connection });
                return;
            };
            queue_a_database_task(
                vm,
                create_function(
                    vm,
                    (entry, NoCells((client_id, connection, old_version, new_version))),
                    |vm, &(entry, NoCells((client_id, connection, old_version, new_version))), ()| {
                        if !entry.close_pending() {
                            fire_a_version_change_event(
                                vm,
                                entry.realm(),
                                "versionchange",
                                entry.event_target(),
                                old_version,
                                new_version,
                            );
                        }
                        send(client_id, Request::VersionChangeHandled { connection });
                    },
                ),
            );
        }
        Event::DeleteSucceeded { request, old_version } => {
            let Some(pending) = remote.deletes.borrow_mut(vm).remove(&(client_id, request)) else {
                return;
            };
            pending.on_complete.call(vm, Ok(old_version));
        }
        Event::DeleteFailed { request, error } => {
            let Some(pending) = remote.deletes.borrow_mut(vm).remove(&(client_id, request)) else {
                return;
            };
            pending.on_complete.call(vm, Err(dom_exception_from(&error)));
        }
        Event::DatabasesListed { request, databases } => {
            let Some(on_complete) = remote.database_lists.borrow_mut(vm).remove(&(client_id, request)) else {
                return;
            };
            on_complete.call(vm, Ok(databases));
        }
        Event::DatabasesFailed { request, error } => {
            let Some(on_complete) = remote.database_lists.borrow_mut(vm).remove(&(client_id, request)) else {
                return;
            };
            on_complete.call(vm, Err(error));
        }
        Event::OperationSucceeded { request, result, .. } => {
            let Some((_, handler)) = remote.results.borrow_mut(vm).remove(&(client_id, request)) else {
                return;
            };
            handler.call(vm, Ok(result));
        }
        Event::OperationFailed { request, error, .. } => {
            let Some((_, handler)) = remote.results.borrow_mut(vm).remove(&(client_id, request)) else {
                return;
            };
            handler.call(vm, Err(error));
        }
        Event::TransactionCompleted { transaction } => {
            let entry = remote.transactions.borrow_mut(vm).remove(&(client_id, transaction));
            if let Some(entry) = entry {
                entry.set_commit_confirmed();
            }
            // NB: The map's borrow ends before the steps run, as they may reach the map again.
            let after = remote.commits.borrow_mut(vm).remove(&(client_id, transaction));
            if let Some(after) = after {
                after.call(vm, ());
            }
        }
        Event::ForcedClose { connection } => {
            let entry = remote
                .connections
                .borrow()
                .get(&(client_id, connection))
                .and_then(GcWeak::get);
            let Some(entry) = entry else {
                return;
            };
            if entry.state() == crate::indexed_db::idb_database::ConnectionState::Closed {
                return;
            }
            // NB: The backend closed the connection already, so the agent only runs the steps for script.
            queue_a_database_task(
                vm,
                create_function(vm, entry, |vm, &entry, ()| {
                    crate::indexed_db::internal::algorithms::close_a_database_connection(vm, entry, None, true);
                }),
            );
        }
        Event::TransactionAborted { transaction } => {
            // NB: An abort the agent asked for already ran its steps. The backend decides on one itself when it can't
            //     write a committing transaction's changes to the profile, which the agent runs the steps of here.
            remote.commits.borrow_mut(vm).remove(&(client_id, transaction));
            forget_results_of(vm, (client_id, transaction));
            // NB: The map's borrow ends before the abort's steps run, which reach the map again.
            let entry = remote.transactions.borrow_mut(vm).remove(&(client_id, transaction));
            if let Some(entry) = entry {
                crate::indexed_db::internal::algorithms::abort_a_transaction(
                    vm,
                    entry,
                    Some(DomException::create(
                        "UnknownError",
                        "The backend aborted the transaction",
                    )),
                );
            }
        }
    }
}

/// Hands the backend a change to the database's shape that an upgrade transaction made. The agent checked it against
/// its own view of the shape already, so the backend refusing it means the two disagree, and the transaction aborts.
pub(crate) fn change_schema(vm: &Vm, transaction: Ref<IDBTransaction>, operation: BackendOperation) {
    perform(
        vm,
        transaction,
        operation,
        crate::indexed_db::internal::function::create_function_with(
            vm,
            transaction,
            |vm, &transaction, result: Result<OperationResult, BackendError>| {
                if let Err(error) = result {
                    crate::indexed_db::internal::algorithms::abort_a_transaction(
                        vm,
                        transaction,
                        Some(dom_exception_from(&error)),
                    );
                }
            },
        ),
    );
}

/// Runs the steps for each of the agent's connections, open or closing.
/// The agent's transactions that haven't finished, in the order they were created, including those of connections
/// that are closing: A connection's close waits for its transactions, which still have to commit on their own.
pub(crate) fn for_each_transaction(vm: &Vm, mut steps: impl FnMut(Ref<IDBTransaction>)) {
    let storage = remote(vm);
    let mut keys: Vec<(u64, u64)> = storage.remote().transactions.borrow().0.keys().copied().collect();
    keys.sort_by_key(|(_, id)| *id);
    for key in keys {
        let transaction = storage.remote().transactions.borrow().get(&key).copied();
        if let Some(transaction) = transaction {
            steps(transaction);
        }
    }
}
