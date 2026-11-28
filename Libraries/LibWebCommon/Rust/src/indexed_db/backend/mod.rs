/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The backend that holds the databases of every agent of a browsing session: their records, the connections to them,
//! and the transactions that read and change them, https://w3c.github.io/IndexedDB/
//!
//! Agents run the parts of the algorithms that need script (cloning values, extracting keys from them, firing events)
//! and hand the backend everything else as requests. The backend answers each one with events, in the order an agent
//! must see them.

pub mod cursor;
pub mod inspection;
pub mod persistence;
pub mod store;

#[cfg(test)]
mod tests;

use std::collections::{BTreeSet, HashMap, HashSet, VecDeque};
use std::rc::Rc;

use libipc_rust::wire::{Decode, Encode};

use crate::indexed_db::Name;

use crate::indexed_db::backend::cursor::{Cursor, Direction, FoundRecord, Source, records_from};
use crate::indexed_db::backend::persistence::Persistence;
use crate::indexed_db::backend::store::{Index, IndexKeys, KeyPath, NamedIndexKeys, ObjectStore, Undo, key_bytes};
use crate::indexed_db::key::Key;
use crate::indexed_db::key_range::{KeyRange, OrderedKey};

/// An agent's connection to the backend, which the backend binds to one storage key.
pub type ClientId = u64;

/// A request, as the agent that made it numbers its requests.
pub type RequestId = u64;

/// A connection to a database, https://w3c.github.io/IndexedDB/#connection, as the backend numbers them.
pub type ConnectionId = u64;

/// A transaction, as the agent that created it numbers its transactions. The backend numbers the upgrade transactions
/// it creates from UPGRADE_TRANSACTION_ID_BASE up, so no agent's number can collide with one of them.
pub type TransactionId = u64;

/// A cursor, as the agent that opened it numbers its cursors.
pub type CursorId = u64;

pub(crate) const UPGRADE_TRANSACTION_ID_BASE: TransactionId = 1 << 63;

/// Milliseconds since the epoch.
fn now() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_or(0, |elapsed| elapsed.as_millis() as u64)
}

const MIB: u64 = 1024 * 1024;

// https://storage.spec.whatwg.org/#storage-quota
// NB: The quota that StorageManager reports for a storage shelf, less what its local storage may take.
const QUOTA_OF_A_STORAGE_KEY: u64 = 10 * 1024 * MIB - 5 * MIB;

/// How many bytes the databases may take, by the measure that storage quotas use: those of one storage key, and those
/// of every storage key together, which bounds the memory that the backend holds them in.
#[derive(Clone, Copy, Debug)]
pub struct Limits {
    pub storage_key: u64,
    pub session: u64,
}

impl Default for Limits {
    fn default() -> Self {
        Self {
            storage_key: QUOTA_OF_A_STORAGE_KEY,
            session: u64::MAX,
        }
    }
}

/// The DOMException name that a failed request or transaction reports.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Encode, Decode)]
#[repr(u8)]
pub enum ErrorKind {
    Abort,
    Constraint,
    Data,
    InvalidState,
    NotFound,
    TransactionInactive,
    Unknown,
    Version,
    QuotaExceeded,
}

#[derive(Clone, Debug, PartialEq, Encode, Decode)]
pub struct Error {
    pub kind: ErrorKind,
    pub message: String,
}

impl Error {
    pub fn new(kind: ErrorKind, message: &str) -> Self {
        Self {
            kind,
            message: message.to_owned(),
        }
    }
}

// https://w3c.github.io/IndexedDB/#transaction-mode
#[derive(Clone, Copy, Debug, PartialEq, Eq, Encode, Decode)]
#[repr(u8)]
pub enum Mode {
    ReadOnly,
    ReadWrite,
    VersionChange,
}

#[derive(Clone, Debug, PartialEq, Encode, Decode)]
pub struct IndexMetadata {
    pub name: Name,
    pub key_path: KeyPath,
    pub unique: bool,
    pub multi_entry: bool,
}

#[derive(Clone, Debug, PartialEq, Encode, Decode)]
pub struct ObjectStoreMetadata {
    pub name: Name,
    pub key_path: Option<KeyPath>,
    pub auto_increment: bool,
    pub indexes: Vec<IndexMetadata>,
}

/// What an agent needs to know of a database to answer its connection's synchronous questions: its version, and the
/// names and shapes of its object stores and their indexes.
#[derive(Clone, Debug, PartialEq, Encode, Decode)]
pub struct DatabaseMetadata {
    pub name: Name,
    pub version: u64,
    pub object_stores: Vec<ObjectStoreMetadata>,
}

/// What a retrieval of multiple items returns, https://w3c.github.io/IndexedDB/#retrieve-multiple-items-from-an-object-store
#[derive(Clone, Copy, Debug, PartialEq, Eq, Encode, Decode)]
#[repr(u8)]
pub enum Retrieve {
    Keys,
    Values,
    Records,
}

/// The index keys of one existing object store record, for an index that an upgrade transaction created.
#[derive(Clone, Encode, Decode)]
pub struct IndexKeyEntry {
    pub primary_key: Key,
    pub keys: Vec<Key>,
}

/// A database's name and version, as https://w3c.github.io/IndexedDB/#dom-idbfactory-databases reports them.
#[derive(Clone, Debug, PartialEq, Encode, Decode)]
pub struct DatabaseInfo {
    pub name: Name,
    pub version: u64,
}

/// An operation of a request, https://w3c.github.io/IndexedDB/#request-construct, that the backend performs when the
/// request's transaction gets to it.
#[derive(Clone, Encode, Decode)]
pub enum Operation {
    Put {
        store: Name,
        value: Vec<u8>,
        key: Option<Key>,
        index_keys: Vec<NamedIndexKeys>,
        no_overwrite: bool,
    },
    Get {
        source: Source,
        range: KeyRange,
    },
    GetKey {
        source: Source,
        range: KeyRange,
    },
    GetAll {
        source: Source,
        range: KeyRange,
        count: Option<u32>,
        direction: Direction,
        retrieve: Retrieve,
    },
    Count {
        source: Source,
        range: KeyRange,
    },
    Delete {
        store: Name,
        range: KeyRange,
    },
    Clear {
        store: Name,
    },
    OpenCursor {
        cursor: CursorId,
        source: Source,
        range: KeyRange,
        direction: Direction,
        key_only: bool,
    },
    IterateCursor {
        cursor: CursorId,
        key: Option<Key>,
        primary_key: Option<Key>,
        count: u32,
    },
    // The changes to a database's shape, which only an upgrade transaction makes. The agent has already checked them
    // against its copy of the database's metadata, as the methods that make them must do synchronously.
    CreateObjectStore {
        name: Name,
        key_path: Option<KeyPath>,
        auto_increment: bool,
    },
    DeleteObjectStore {
        name: Name,
    },
    RenameObjectStore {
        name: Name,
        new_name: Name,
    },
    CreateIndex {
        store: Name,
        name: Name,
        key_path: KeyPath,
        unique: bool,
        multi_entry: bool,
    },
    DeleteIndex {
        store: Name,
        name: Name,
    },
    RenameIndex {
        store: Name,
        name: Name,
        new_name: Name,
    },
    /// Hands the agent the records of an object store, from which it computes the records of a new index: the start of
    /// https://w3c.github.io/IndexedDB/#dom-idbobjectstore-createindex for an object store that holds records.
    PopulateIndex {
        store: Name,
        index: Name,
    },
    /// The index records of the object store records that a PopulateIndex handed the agent.
    AddIndexKeys {
        store: Name,
        index: Name,
        entries: Vec<IndexKeyEntry>,
    },
}

impl Operation {
    /// The object store the operation reads or changes, for one that names it. A cursor's object store is the one it
    /// was opened on.
    fn store(&self) -> Option<&Name> {
        match self {
            Operation::Put { store, .. }
            | Operation::Delete { store, .. }
            | Operation::Clear { store }
            | Operation::CreateIndex { store, .. }
            | Operation::DeleteIndex { store, .. }
            | Operation::RenameIndex { store, .. }
            | Operation::PopulateIndex { store, .. }
            | Operation::AddIndexKeys { store, .. } => Some(store),
            Operation::Get { source, .. }
            | Operation::GetKey { source, .. }
            | Operation::GetAll { source, .. }
            | Operation::Count { source, .. }
            | Operation::OpenCursor { source, .. } => Some(source.store_name()),
            Operation::IterateCursor { .. }
            | Operation::CreateObjectStore { .. }
            | Operation::DeleteObjectStore { .. }
            | Operation::RenameObjectStore { .. } => None,
        }
    }
}

/// The result of an operation that succeeded.
#[derive(Clone, Encode, Decode)]
pub enum OperationResult {
    Undefined,
    Key(Key),
    OptionalKey(Option<Key>),
    Value(Option<FoundRecord>),
    Count(u64),
    Records(Vec<FoundRecord>),
    Cursor(Option<FoundRecord>),
}

/// What an agent asks of the backend.
#[derive(Clone, Encode, Decode)]
pub enum Request {
    Open {
        request: RequestId,
        name: Name,
        version: Option<u64>,
    },
    DeleteDatabase {
        request: RequestId,
        name: Name,
    },
    Databases {
        request: RequestId,
    },
    /// The agent ran https://w3c.github.io/IndexedDB/#close-a-database-connection: its close pending flag is set.
    Close {
        connection: ConnectionId,
    },
    /// The agent fired the versionchange event that a VersionChange event asked for.
    VersionChangeHandled {
        connection: ConnectionId,
    },
    /// The agent queued the success event of an open request that succeeded, after which the connection queue's next
    /// request may go on: https://w3c.github.io/IndexedDB/#open-a-database-connection step 3 waits until every
    /// previous request in the queue was processed.
    OpenHandled {
        request: RequestId,
    },
    CreateTransaction {
        connection: ConnectionId,
        transaction: TransactionId,
        scope: Vec<Name>,
        mode: Mode,
    },
    Operation {
        transaction: TransactionId,
        request: RequestId,
        operation: Operation,
    },
    /// The transaction is to commit once the operations the agent sent before this have run.
    Commit {
        transaction: TransactionId,
    },
    Abort {
        transaction: TransactionId,
    },
}

/// What the backend tells an agent.
#[derive(Clone, Encode, Decode)]
pub enum Event {
    UpgradeNeeded {
        request: RequestId,
        connection: ConnectionId,
        transaction: TransactionId,
        old_version: u64,
        metadata: DatabaseMetadata,
    },
    OpenSucceeded {
        request: RequestId,
        connection: ConnectionId,
        metadata: DatabaseMetadata,
    },
    OpenFailed {
        request: RequestId,
        error: Error,
    },
    Blocked {
        request: RequestId,
        old_version: u64,
        new_version: Option<u64>,
    },
    VersionChange {
        connection: ConnectionId,
        old_version: u64,
        new_version: Option<u64>,
    },
    DeleteSucceeded {
        request: RequestId,
        old_version: u64,
    },
    DeleteFailed {
        request: RequestId,
        error: Error,
    },
    DatabasesListed {
        request: RequestId,
        databases: Vec<DatabaseInfo>,
    },
    DatabasesFailed {
        request: RequestId,
        error: Error,
    },
    OperationSucceeded {
        transaction: TransactionId,
        request: RequestId,
        result: OperationResult,
    },
    OperationFailed {
        transaction: TransactionId,
        request: RequestId,
        error: Error,
    },
    TransactionCompleted {
        transaction: TransactionId,
    },
    TransactionAborted {
        transaction: TransactionId,
    },
    /// The backend closed a connection with the forced flag, as when the user clears a site's data: Its transactions
    /// abort, and it gets a close event.
    /// https://w3c.github.io/IndexedDB/#close-a-database-connection
    ForcedClose {
        connection: ConnectionId,
    },
}

const UNREADABLE_DATABASE: &str = "The database couldn't be read from the profile";

// https://w3c.github.io/IndexedDB/#database-construct
struct Database {
    name: Name,
    version: u64,
    object_stores: Vec<ObjectStore>,

    // The number the next object store or index gets as its id.
    next_id: u64,

    // The connections associated with the database, open or close pending.
    connections: Vec<ConnectionId>,

    // The transactions of the database that have not finished, in the order they were created.
    transactions: Vec<(ClientId, TransactionId)>,
}

impl Database {
    fn take_id(&mut self) -> u64 {
        let id = self.next_id;
        self.next_id += 1;
        id
    }

    fn new(name: Name) -> Self {
        Self {
            name,
            version: 0,
            object_stores: Vec::new(),
            next_id: 1,
            connections: Vec::new(),
            transactions: Vec::new(),
        }
    }

    fn metadata(&self) -> DatabaseMetadata {
        Self::metadata_of(&self.name, self.version, &self.object_stores)
    }

    /// The shape that the database had before an upgrade that's under way, or has if none is.
    fn committed_metadata(&self, transactions: &HashMap<(ClientId, TransactionId), Transaction>) -> DatabaseMetadata {
        let snapshot = self
            .transactions
            .iter()
            .filter_map(|id| transactions.get(id))
            .find_map(|transaction| transaction.snapshot.as_ref());
        match snapshot {
            Some(snapshot) => Self::metadata_of(&self.name, snapshot.version, &snapshot.object_stores),
            None => self.metadata(),
        }
    }

    fn metadata_of(name: &Name, version: u64, object_stores: &[ObjectStore]) -> DatabaseMetadata {
        DatabaseMetadata {
            name: name.clone(),
            version,
            object_stores: object_stores
                .iter()
                .map(|store| ObjectStoreMetadata {
                    name: store.name.clone(),
                    key_path: store.key_path.clone(),
                    auto_increment: store.key_generator.is_some(),
                    indexes: store
                        .indexes
                        .iter()
                        .map(|index| IndexMetadata {
                            name: index.name.clone(),
                            key_path: index.key_path.clone(),
                            unique: index.unique,
                            multi_entry: index.multi_entry,
                        })
                        .collect(),
                })
                .collect(),
        }
    }

    fn store(&self, name: &Name) -> Result<&ObjectStore, Error> {
        self.object_stores
            .iter()
            .find(|store| store.name == *name)
            .ok_or_else(|| Error::new(ErrorKind::NotFound, "No object store with this name"))
    }

    fn store_mut(&mut self, name: &Name) -> Result<&mut ObjectStore, Error> {
        self.object_stores
            .iter_mut()
            .find(|store| store.name == *name)
            .ok_or_else(|| Error::new(ErrorKind::NotFound, "No object store with this name"))
    }
}

// https://w3c.github.io/IndexedDB/#connection
struct Connection {
    client: ClientId,
    storage_key: String,
    database: Name,
    close_pending: bool,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum TransactionState {
    /// Created, and waiting for transactions it overlaps to finish, https://w3c.github.io/IndexedDB/#transaction-scheduling
    Waiting,
    Running,
}

/// What the database looked like when an upgrade transaction started, which aborting it puts back.
struct UpgradeSnapshot {
    version: u64,
    object_stores: Vec<ObjectStore>,
}

// https://w3c.github.io/IndexedDB/#transaction-construct
struct Transaction {
    connection: ConnectionId,
    storage_key: String,
    database: Name,
    scope: Vec<Name>,
    mode: Mode,
    state: TransactionState,
    operations: VecDeque<(RequestId, Operation)>,
    commit_requested: bool,
    undo: Vec<Undo>,
    // The records that the undo log holds an entry for, and the bytes of the values those entries keep: Only a record's
    // first entry is kept, since it holds what the record was before the transaction.
    undone: BTreeSet<(u64, OrderedKey)>,
    retained: u64,
    snapshot: Option<UpgradeSnapshot>,
    cursors: HashMap<CursorId, Cursor>,
}

/// The request at the head of a connection queue, https://w3c.github.io/IndexedDB/#connection-queues, and how far it
/// has got.
enum Pending {
    Open {
        client: ClientId,
        request: RequestId,
        name: Name,
        version: Option<u64>,
        connection: Option<ConnectionId>,
    },
    Delete {
        client: ClientId,
        request: RequestId,
        name: Name,
    },
}

enum Stage {
    /// Not yet started.
    Queued,
    /// The versionchange events went out, and the backend waits for the agents to have fired them.
    WaitingForVersionChangeEvents { unhandled: HashSet<ConnectionId> },
    /// The agents fired the versionchange events, and the backend waits for the other connections to close.
    WaitingForConnectionsToClose { blocked_sent: bool },
    /// The upgrade transaction runs.
    Upgrading { connection: ConnectionId },
    /// The open request succeeded, and the backend waits for the agent to have queued its success event.
    WaitingForOpenHandled,
}

struct QueueEntry {
    pending: Pending,
    stage: Stage,
}

#[derive(Default)]
pub struct Backend {
    clients: HashMap<ClientId, String>,
    databases: HashMap<String, Vec<Database>>,
    connections: HashMap<ConnectionId, Connection>,
    transactions: HashMap<(ClientId, TransactionId), Transaction>,
    queues: HashMap<(String, Name), VecDeque<QueueEntry>>,
    next_connection_id: ConnectionId,
    next_upgrade_transaction_id: TransactionId,
    events: Vec<(ClientId, Event)>,
    // The databases whose waiting transactions may now start, and whether a loop is already starting them.
    to_schedule: VecDeque<(String, Name)>,
    scheduling: bool,
    // The acknowledgements of versionchange events still to come for requests that went away before them, by
    // connection, which must not count for a later request's events.
    stale_acknowledgements: HashMap<ConnectionId, u32>,
    // Where the databases are kept between runs, if anywhere, and the storage keys whose databases it handed over.
    persistence: Option<Box<dyn Persistence>>,
    loaded: HashSet<String>,
    // The databases of loaded storage keys whose rows this version can't read, and the storage keys whose rows
    // couldn't be read at all.
    unreadable: HashSet<(String, Name)>,
    unavailable: HashSet<String>,
    limits: Limits,
    // When each storage key's databases last changed in this run, in milliseconds since the epoch.
    written: HashMap<String, u64>,
    // The storage key whose site data is being cleared, whose connection queues wait until that's done.
    clearing: Option<String>,
}

impl Backend {
    pub fn new() -> Self {
        Self {
            next_upgrade_transaction_id: UPGRADE_TRANSACTION_ID_BASE,
            ..Default::default()
        }
    }

    /// A backend that keeps its databases between runs in the persistence.
    pub fn with_persistence(persistence: Box<dyn Persistence>) -> Self {
        Self {
            persistence: Some(persistence),
            ..Self::new()
        }
    }

    pub fn set_limits(&mut self, limits: Limits) {
        self.limits = limits;
    }

    /// The storage keys whose databases changed in this run at or after the time, in milliseconds since the epoch.
    pub fn storage_keys_written_since(&self, since: u64) -> Vec<String> {
        self.written
            .iter()
            .filter(|(_, written)| **written >= since)
            .map(|(storage_key, _)| storage_key.clone())
            .collect()
    }

    /// Deletes every database of the storage key, as when the user clears the site's data: Each connection to one is
    /// closed with the forced flag, which aborts its transactions, and the databases go, along with their rows.
    /// Returns whether every database's rows went; a database whose rows couldn't be deleted stays.
    pub fn clear_storage_key(&mut self, storage_key: &str) -> bool {
        // NB: The connection queues wait until the clear is done: A request that one of the steps below let go on
        //     would otherwise run against a database that's about to go.
        self.clearing = Some(storage_key.to_owned());
        let connections: Vec<_> = self
            .connections
            .iter()
            .filter(|(_, connection)| connection.storage_key == storage_key)
            .map(|(id, connection)| (*id, connection.client))
            .collect();
        for (connection, client) in &connections {
            self.events.push((
                *client,
                Event::ForcedClose {
                    connection: *connection,
                },
            ));
            if let Some(entry) = self.connections.get_mut(connection) {
                entry.close_pending = true;
            }
        }
        self.abort_transactions_of_storage_key(storage_key);
        self.fail_started_opens_of_storage_key(storage_key);

        let names: Vec<Name> = self
            .databases
            .get(storage_key)
            .map(|databases| databases.iter().map(|database| database.name.clone()).collect())
            .unwrap_or_default();
        let mut deleted_all = true;
        for name in names {
            if self.persist_deletion(storage_key, &name) {
                self.forget_database(storage_key, &name);
            } else {
                deleted_all = false;
            }
        }
        let unreadable: Vec<Name> = self
            .unreadable
            .iter()
            .filter(|(key, _)| key == storage_key)
            .map(|(_, name)| name.clone())
            .collect();
        for name in unreadable {
            if self.persist_deletion(storage_key, &name) {
                self.unreadable.remove(&(storage_key.to_owned(), name));
            } else {
                deleted_all = false;
            }
        }

        for (connection, _) in connections {
            self.close_connection(connection);
        }
        if deleted_all {
            self.written.remove(storage_key);
        }

        // NB: The requests that hadn't started go on now, so an open among them creates its database anew.
        self.clearing = None;
        let keys: Vec<_> = self.queues.keys().filter(|key| key.0 == storage_key).cloned().collect();
        for key in keys {
            self.advance_queue(&key);
        }
        deleted_all
    }

    /// Fails the opens at the heads of a storage key's connection queues that wait for other connections, whose
    /// databases are about to go: Their upgrades would run against a database that no longer exists.
    fn fail_started_opens_of_storage_key(&mut self, storage_key: &str) {
        let keys: Vec<_> = self.queues.keys().filter(|key| key.0 == storage_key).cloned().collect();
        for key in keys {
            let Some(queue) = self.queues.get_mut(&key) else {
                continue;
            };
            let Some(QueueEntry {
                pending:
                    Pending::Open {
                        client,
                        request,
                        connection,
                        ..
                    },
                stage:
                    stage @ (Stage::WaitingForVersionChangeEvents { .. } | Stage::WaitingForConnectionsToClose { .. }),
            }) = queue.front()
            else {
                continue;
            };
            let (client, request, connection) = (*client, *request, *connection);
            if let Stage::WaitingForVersionChangeEvents { unhandled } = stage {
                for other in unhandled.clone() {
                    *self.stale_acknowledgements.entry(other).or_default() += 1;
                }
            }
            queue.pop_front();
            self.events.push((
                client,
                Event::OpenFailed {
                    request,
                    error: Error::new(ErrorKind::Abort, "The site's data was cleared"),
                },
            ));
            if let Some(connection) = connection {
                self.connections.remove(&connection);
                if let Some(database) = self.database_mut(&key.0, &key.1) {
                    database.connections.retain(|other| *other != connection);
                }
            }
        }
    }

    /// Aborts the transactions on a storage key's databases, the waiting ones first: Aborting a running one would
    /// otherwise start one that waits for it.
    fn abort_transactions_of_storage_key(&mut self, storage_key: &str) {
        let mut transactions: Vec<_> = self
            .transactions
            .iter()
            .filter(|(_, transaction)| transaction.storage_key == storage_key)
            .map(|(id, transaction)| (transaction.state == TransactionState::Running, *id))
            .collect();
        transactions.sort();
        for (_, (client, transaction)) in transactions {
            self.abort_transaction(client, transaction);
        }
    }

    /// Binds a client to a storage key, which every request the client makes acts within. The caller numbers its
    /// clients, and never reuses a number.
    pub fn connect_client(&mut self, client: ClientId, storage_key: String) {
        self.load_storage_key(&storage_key);
        self.clients.insert(client, storage_key);
    }

    /// Forgets a client that went away: its transactions abort, and its connections close.
    pub fn disconnect_client(&mut self, client: ClientId) {
        // NB: First, the client's requests that haven't started an upgrade go, so that closing its connections below
        //     can't start one for it: An open that waits for the client's own connection would otherwise go on once
        //     that closes, with an upgrade transaction that nothing would ever end.
        self.drop_requests_of(client, false);

        // NB: The client's waiting transactions abort first: Aborting a running one would otherwise start one that
        //     waits for it, which could commit before its own abort came.
        let mut transactions: Vec<_> = self
            .transactions
            .iter()
            .filter(|((owner, _), _)| *owner == client)
            .map(|(id, transaction)| (transaction.state == TransactionState::Running, id.1))
            .collect();
        transactions.sort();
        for (_, transaction) in transactions {
            self.abort_transaction(client, transaction);
        }
        let connections: Vec<_> = self
            .connections
            .iter()
            .filter(|(_, connection)| connection.client == client)
            .map(|(id, _)| *id)
            .collect();
        for connection in connections {
            self.close_connection(connection);
        }
        // NB: An upgrade the client ran ended with its transaction's abort above, and waits only to hand the client a
        //     result nobody can receive.
        self.drop_requests_of(client, true);
        self.clients.remove(&client);
        let keys: Vec<_> = self.queues.keys().cloned().collect();
        for key in keys {
            self.advance_queue(&key);
        }
        self.events.retain(|(target, _)| *target != client);
    }

    /// Removes a client's requests from the connection queues: all of them, or those that haven't started an upgrade.
    fn drop_requests_of(&mut self, client: ClientId, upgrading_too: bool) {
        let mut stale = Vec::new();
        for queue in self.queues.values_mut() {
            queue.retain(|entry| {
                let (Pending::Open { client: owner, .. } | Pending::Delete { client: owner, .. }) = &entry.pending;
                let keep = *owner != client || (!upgrading_too && matches!(entry.stage, Stage::Upgrading { .. }));
                if !keep && let Stage::WaitingForVersionChangeEvents { unhandled } = &entry.stage {
                    stale.extend(unhandled.iter().copied());
                }
                keep
            });
        }
        for connection in stale {
            *self.stale_acknowledgements.entry(connection).or_default() += 1;
        }
    }

    /// The events since the last call, each with the client it is for, in the order they happened.
    pub fn take_events(&mut self) -> Vec<(ClientId, Event)> {
        core::mem::take(&mut self.events)
    }

    pub fn handle(&mut self, client: ClientId, request: Request) {
        let Some(storage_key) = self.clients.get(&client).cloned() else {
            return;
        };
        match request {
            Request::Open { request, name, version } => {
                // NB: The agent throws a TypeError for version 0 before it asks; one that sent it misbehaved.
                if version == Some(0) {
                    self.events.push((
                        client,
                        Event::OpenFailed {
                            request,
                            error: Error::new(ErrorKind::Unknown, "The version is 0"),
                        },
                    ));
                    return;
                }
                self.enqueue(
                    &storage_key,
                    name.clone(),
                    Pending::Open {
                        client,
                        request,
                        name,
                        version,
                        connection: None,
                    },
                );
            }
            Request::DeleteDatabase { request, name } => {
                self.enqueue(&storage_key, name.clone(), Pending::Delete { client, request, name });
            }
            Request::Databases { request } => {
                // https://w3c.github.io/IndexedDB/#dom-idbfactory-databases
                // 2. If the databases' names and versions couldn't be determined, reject the promise.
                if self.unavailable.contains(&storage_key) {
                    self.events.push((
                        client,
                        Event::DatabasesFailed {
                            request,
                            error: Error::new(ErrorKind::Unknown, UNREADABLE_DATABASE),
                        },
                    ));
                    return;
                }
                // https://w3c.github.io/IndexedDB/#dom-idbfactory-databases
                // NB: A database whose first upgrade has not committed has version 0, which no caller is to see.
                // NB: A database's version is the committed one, so an upgrade under way doesn't show yet.
                let committed_version = |database: &Database| {
                    database
                        .transactions
                        .iter()
                        .filter_map(|id| self.transactions.get(id))
                        .find_map(|transaction| transaction.snapshot.as_ref().map(|snapshot| snapshot.version))
                        .unwrap_or(database.version)
                };
                let databases = self
                    .databases
                    .get(&storage_key)
                    .map(|databases| {
                        databases
                            .iter()
                            .map(|database| DatabaseInfo {
                                name: database.name.clone(),
                                version: committed_version(database),
                            })
                            .filter(|info| info.version > 0)
                            .collect()
                    })
                    .unwrap_or_default();
                self.events
                    .push((client, Event::DatabasesListed { request, databases }));
            }
            Request::Close { connection } => {
                if self
                    .connections
                    .get(&connection)
                    .is_some_and(|entry| entry.client == client)
                {
                    self.request_close(connection);
                }
            }
            Request::OpenHandled { request } => {
                self.open_handled(client, request);
            }
            Request::VersionChangeHandled { connection } => {
                if self
                    .connections
                    .get(&connection)
                    .is_some_and(|entry| entry.client == client)
                {
                    self.version_change_handled(connection);
                }
            }
            Request::CreateTransaction {
                connection,
                transaction,
                scope,
                mode,
            } => {
                self.create_transaction(client, connection, transaction, scope, mode);
            }
            Request::Operation {
                transaction,
                request,
                operation,
            } => {
                if let Some(entry) = self.transactions.get_mut(&(client, transaction)) {
                    entry.operations.push_back((request, operation));
                    self.run_transaction(client, transaction);
                }
            }
            Request::Commit { transaction } => {
                if let Some(entry) = self.transactions.get_mut(&(client, transaction)) {
                    entry.commit_requested = true;
                    self.run_transaction(client, transaction);
                }
            }
            Request::Abort { transaction } => {
                self.abort_transaction(client, transaction);
            }
        }
    }

    fn database(&self, storage_key: &str, name: &Name) -> Option<&Database> {
        self.databases
            .get(storage_key)
            .and_then(|databases| databases.iter().find(|database| database.name == *name))
    }

    fn database_mut(&mut self, storage_key: &str, name: &Name) -> Option<&mut Database> {
        self.databases
            .get_mut(storage_key)
            .and_then(|databases| databases.iter_mut().find(|database| database.name == *name))
    }

    /// Deletes a database, and its rows, and returns whether it could.
    fn remove_database(&mut self, storage_key: &str, name: &Name) -> bool {
        if !self.persist_deletion(storage_key, name) {
            return false;
        }
        self.written.insert(storage_key.to_owned(), now());
        self.forget_database(storage_key, name);
        true
    }

    /// Drops a database that has no rows: one whose first upgrade never committed.
    fn forget_database(&mut self, storage_key: &str, name: &Name) {
        if let Some(databases) = self.databases.get_mut(storage_key) {
            databases.retain(|database| database.name != *name);
        }
    }

    fn enqueue(&mut self, storage_key: &str, name: Name, pending: Pending) {
        let key = (storage_key.to_owned(), name);
        self.queues.entry(key.clone()).or_default().push_back(QueueEntry {
            pending,
            stage: Stage::Queued,
        });
        self.advance_queue(&key);
    }

    /// Runs the request at the head of a connection queue as far as it can get, and the ones after it once it is done.
    fn advance_queue(&mut self, key: &(String, Name)) {
        if self.clearing.as_deref() == Some(key.0.as_str()) {
            return;
        }
        loop {
            let Some(entry) = self.queues.get_mut(key).and_then(|queue| queue.front_mut()) else {
                self.queues.remove(key);
                return;
            };
            let done = match entry.stage {
                Stage::Queued => self.start_queue_entry(key),
                Stage::WaitingForVersionChangeEvents { .. } => self.check_version_change_events(key),
                Stage::WaitingForConnectionsToClose { .. } => self.check_connections_closed(key),
                Stage::Upgrading { .. } | Stage::WaitingForOpenHandled => false,
            };
            if !done {
                return;
            }
            if let Some(queue) = self.queues.get_mut(key) {
                queue.pop_front();
            }
        }
    }

    fn other_open_connections(&self, key: &(String, Name), except: Option<ConnectionId>) -> Vec<ConnectionId> {
        self.database(&key.0, &key.1)
            .map(|database| {
                database
                    .connections
                    .iter()
                    .copied()
                    .filter(|connection| Some(*connection) != except)
                    .collect()
            })
            .unwrap_or_default()
    }

    /// Starts the request at the head of a connection queue, and returns whether it is already done.
    fn start_queue_entry(&mut self, key: &(String, Name)) -> bool {
        let entry = self
            .queues
            .get(key)
            .and_then(|queue| queue.front())
            .expect("the queue has a head");
        match &entry.pending {
            // https://w3c.github.io/IndexedDB/#open-a-database-connection
            Pending::Open {
                client,
                request,
                name,
                version,
                ..
            } => {
                let (client, request, name, version) = (*client, *request, name.clone(), *version);
                if self.unavailable.contains(&key.0) || self.unreadable.contains(&(key.0.clone(), name.clone())) {
                    self.events.push((
                        client,
                        Event::OpenFailed {
                            request,
                            error: Error::new(ErrorKind::Unknown, UNREADABLE_DATABASE),
                        },
                    ));
                    return true;
                }

                // 4. Let db be the database named name in storageKey, or null otherwise.
                // 5. If version is undefined, let version be 1 if db is null, or db's version otherwise.
                // 6. If db is null, let db be a new database with name name, version 0 (zero), and with no object
                //    stores.
                let existed = self.database(&key.0, &name).is_some();
                if !existed {
                    self.databases
                        .entry(key.0.clone())
                        .or_default()
                        .push(Database::new(name.clone()));
                }
                let current_version = self.database(&key.0, &name).expect("the database exists").version;
                let version = version.unwrap_or(if current_version == 0 { 1 } else { current_version });

                // 7. If db's version is greater than version, return a newly created "VersionError" DOMException and
                //    abort these steps.
                if current_version > version {
                    self.events.push((
                        client,
                        Event::OpenFailed {
                            request,
                            error: Error::new(
                                ErrorKind::Version,
                                "The requested version is less than the version of the database",
                            ),
                        },
                    ));
                    return true;
                }

                // 8. Let connection be a new connection to db.
                // 9. Set connection's version to version.
                self.next_connection_id += 1;
                let connection = self.next_connection_id;
                self.connections.insert(
                    connection,
                    Connection {
                        client,
                        storage_key: key.0.clone(),
                        database: name.clone(),
                        close_pending: false,
                    },
                );
                self.database_mut(&key.0, &name)
                    .expect("the database exists")
                    .connections
                    .push(connection);

                // 10. If db's version is less than version, then:
                if current_version < version {
                    let entry = self.queues.get_mut(key).and_then(|queue| queue.front_mut()).unwrap();
                    entry.pending = Pending::Open {
                        client,
                        request,
                        name,
                        version: Some(version),
                        connection: Some(connection),
                    };
                    self.begin_version_change(key, Some(connection), current_version, Some(version));
                    return self.check_version_change_events(key);
                }

                // 11. Return connection.
                let metadata = self.database(&key.0, &name).expect("the database exists").metadata();
                self.events.push((
                    client,
                    Event::OpenSucceeded {
                        request,
                        connection,
                        metadata,
                    },
                ));
                let entry = self.queues.get_mut(key).and_then(|queue| queue.front_mut()).unwrap();
                entry.stage = Stage::WaitingForOpenHandled;
                false
            }

            // https://w3c.github.io/IndexedDB/#delete-a-database
            Pending::Delete { client, request, name } => {
                let (client, request, name) = (*client, *request, name.clone());
                if self.unavailable.contains(&key.0) {
                    self.events.push((
                        client,
                        Event::DeleteFailed {
                            request,
                            error: Error::new(ErrorKind::Unknown, UNREADABLE_DATABASE),
                        },
                    ));
                    return true;
                }
                // NB: A database whose rows can't be read can still go: Deleting its rows needs none of them read.
                //     Its version is unknown, so the request reports 0.
                if self.unreadable.contains(&(key.0.clone(), name.clone())) {
                    let event = if self.persist_deletion(&key.0, &name) {
                        self.unreadable.remove(&(key.0.clone(), name.clone()));
                        Event::DeleteSucceeded {
                            request,
                            old_version: 0,
                        }
                    } else {
                        Event::DeleteFailed {
                            request,
                            error: Error::new(ErrorKind::Unknown, "The database couldn't be deleted from the profile"),
                        }
                    };
                    self.events.push((client, event));
                    return true;
                }

                // 4. Let db be the database named name in storageKey, if one exists. Otherwise, return 0 (zero).
                let Some(database) = self.database(&key.0, &name) else {
                    self.events.push((
                        client,
                        Event::DeleteSucceeded {
                            request,
                            old_version: 0,
                        },
                    ));
                    return true;
                };
                let version = database.version;
                self.begin_version_change(key, None, version, None);
                self.check_version_change_events(key)
            }
        }
    }

    /// Fires versionchange at every other open connection of the database, and starts waiting for the agents to have
    /// fired them.
    fn begin_version_change(
        &mut self,
        key: &(String, Name),
        except: Option<ConnectionId>,
        old_version: u64,
        new_version: Option<u64>,
    ) {
        // Let openConnections be the set of all connections, except connection, associated with db.
        // For each entry of openConnections that does not have its close pending flag set to true, queue a database
        // task to fire a version change event named versionchange at entry with db's version and version.
        let mut unhandled = HashSet::new();
        for connection in self.other_open_connections(key, except) {
            let entry = &self.connections[&connection];
            if entry.close_pending {
                continue;
            }
            unhandled.insert(connection);
            self.events.push((
                entry.client,
                Event::VersionChange {
                    connection,
                    old_version,
                    new_version,
                },
            ));
        }
        let entry = self.queues.get_mut(key).and_then(|queue| queue.front_mut()).unwrap();
        entry.stage = Stage::WaitingForVersionChangeEvents { unhandled };
    }

    fn version_change_handled(&mut self, connection: ConnectionId) {
        // NB: The agent acknowledges a connection's versionchange events in the order they went out.
        if let Some(count) = self.stale_acknowledgements.get_mut(&connection) {
            *count -= 1;
            if *count == 0 {
                self.stale_acknowledgements.remove(&connection);
            }
            return;
        }
        let Some(entry) = self.connections.get(&connection) else {
            return;
        };
        let key = (entry.storage_key.clone(), entry.database.clone());
        if let Some(Stage::WaitingForVersionChangeEvents { unhandled, .. }) = self
            .queues
            .get_mut(&key)
            .and_then(|queue| queue.front_mut())
            .map(|entry| &mut entry.stage)
        {
            unhandled.remove(&connection);
        }
        self.advance_queue(&key);
    }

    /// Whether the version change at the head of a queue can go on, now that its versionchange events were fired.
    fn check_version_change_events(&mut self, key: &(String, Name)) -> bool {
        let entry = self.queues.get_mut(key).and_then(|queue| queue.front_mut()).unwrap();
        let Stage::WaitingForVersionChangeEvents { unhandled, .. } = &mut entry.stage else {
            unreachable!("the queue's head waits for versionchange events");
        };
        let still_open: Vec<ConnectionId> = unhandled.iter().copied().collect();
        if still_open
            .iter()
            .any(|connection| self.connections.contains_key(connection))
        {
            return false;
        }
        entry.stage = Stage::WaitingForConnectionsToClose { blocked_sent: false };
        self.check_connections_closed(key)
    }

    /// Whether the version change at the head of a queue can go on, once no other connection to its database is open.
    fn check_connections_closed(&mut self, key: &(String, Name)) -> bool {
        let (except, client, request, old_version, new_version) = {
            let entry = self.queues.get(key).and_then(|queue| queue.front()).unwrap();
            let version = self
                .database(&key.0, &key.1)
                .map(|database| database.version)
                .unwrap_or(0);
            match &entry.pending {
                Pending::Open {
                    client,
                    request,
                    version: new_version,
                    connection,
                    ..
                } => (*connection, *client, *request, version, *new_version),
                Pending::Delete { client, request, .. } => (None, *client, *request, version, None),
            }
        };

        // If any of the connections in openConnections are still not closed, queue a database task to fire a version
        // change event named blocked at request with db's version and version.
        // Wait until all connections in openConnections are closed.
        let others = self.other_open_connections(key, except);
        if !others.is_empty() {
            // AD-HOC: The spec fires blocked while a connection whose close pending flag is set still waits for its
            //         transactions to finish. But a page that closed every other connection has done all it can, so
            //         blocked fires only for a connection that hasn't been asked to close, as other engines do.
            let any_not_closing = others.iter().any(|connection| {
                self.connections
                    .get(connection)
                    .is_some_and(|entry| !entry.close_pending)
            });
            let entry = self.queues.get_mut(key).and_then(|queue| queue.front_mut()).unwrap();
            let Stage::WaitingForConnectionsToClose { blocked_sent } = &mut entry.stage else {
                unreachable!("the queue's head waits for connections to close");
            };
            if !*blocked_sent && any_not_closing {
                *blocked_sent = true;
                self.events.push((
                    client,
                    Event::Blocked {
                        request,
                        old_version,
                        new_version,
                    },
                ));
            }
            return false;
        }

        let entry = self.queues.get(key).and_then(|queue| queue.front()).unwrap();
        match &entry.pending {
            Pending::Open { version, .. } => {
                let connection = except.expect("the opening connection exists");
                let version = version.expect("the version is known");
                self.run_upgrade(key, client, request, connection, version);
                false
            }
            Pending::Delete { .. } => {
                // https://w3c.github.io/IndexedDB/#delete-a-database
                // 10. Let version be db's version.
                // 11. Delete db. If this fails for any reason, return an appropriate error (e.g. "QuotaExceededError"
                //     or "UnknownError" DOMException).
                // 12. Return version.
                let event = if self.remove_database(&key.0, &key.1) {
                    Event::DeleteSucceeded { request, old_version }
                } else {
                    Event::DeleteFailed {
                        request,
                        error: Error::new(ErrorKind::Unknown, "The database couldn't be deleted from the profile"),
                    }
                };
                self.events.push((client, event));
                true
            }
        }
    }

    // https://w3c.github.io/IndexedDB/#upgrade-a-database
    fn run_upgrade(
        &mut self,
        key: &(String, Name),
        client: ClientId,
        request: RequestId,
        connection: ConnectionId,
        version: u64,
    ) {
        self.next_upgrade_transaction_id += 1;
        let transaction = self.next_upgrade_transaction_id;
        let database = self.database_mut(&key.0, &key.1).expect("the database exists");

        // 3. Let transaction be a new upgrade transaction with connection used as connection.
        // 4. Set transaction's scope to connection's object store set.
        let snapshot = UpgradeSnapshot {
            version: database.version,
            object_stores: database.object_stores.clone(),
        };
        let old_version = database.version;

        // 6. Set db's version to version. This change is considered part of the transaction, and so if the
        //    transaction is aborted, this change is reverted.
        database.version = version;
        let metadata = database.metadata();

        database.transactions.push((client, transaction));
        self.transactions.insert(
            (client, transaction),
            Transaction {
                connection,
                storage_key: key.0.clone(),
                database: key.1.clone(),
                scope: Vec::new(),
                mode: Mode::VersionChange,
                state: TransactionState::Running,
                operations: VecDeque::new(),
                commit_requested: false,
                undone: BTreeSet::new(),
                retained: 0,
                undo: Vec::new(),
                snapshot: Some(snapshot),
                cursors: HashMap::new(),
            },
        );
        let entry = self.queues.get_mut(key).and_then(|queue| queue.front_mut()).unwrap();
        entry.stage = Stage::Upgrading { connection };

        // 10. Queue a database task to run these steps: fire a version change event named upgradeneeded at request
        //     with old version and version.
        self.events.push((
            client,
            Event::UpgradeNeeded {
                request,
                connection,
                transaction,
                old_version,
                metadata,
            },
        ));
    }

    /// The upgrade transaction at the head of a queue finished; the open request succeeds or fails with it.
    fn finish_upgrade(&mut self, key: &(String, Name), committed: bool) {
        let Some(entry) = self.queues.get(key).and_then(|queue| queue.front()) else {
            return;
        };
        let (Pending::Open { client, request, .. }, Stage::Upgrading { connection }) = (&entry.pending, &entry.stage)
        else {
            return;
        };
        let (client, request, connection) = (*client, *request, *connection);
        // 10.7. If connection was closed, return a newly created "AbortError" DOMException and abort these steps.
        // NB: A connection that script closed during upgradeneeded is closed once its upgrade transaction is done.
        let open = self
            .connections
            .get(&connection)
            .is_some_and(|entry| !entry.close_pending);
        if committed && open {
            let metadata = self.database(&key.0, &key.1).expect("the database exists").metadata();
            self.events.push((
                client,
                Event::OpenSucceeded {
                    request,
                    connection,
                    metadata,
                },
            ));
            let entry = self.queues.get_mut(key).and_then(|queue| queue.front_mut()).unwrap();
            entry.stage = Stage::WaitingForOpenHandled;
            return;
        }

        // https://w3c.github.io/IndexedDB/#open-a-database-connection
        // 10.4. If the upgrade transaction was aborted, run the steps to close a database connection with connection,
        //       return a newly created "AbortError" DOMException and abort these steps.
        self.close_connection(connection);
        self.events.push((
            client,
            Event::OpenFailed {
                request,
                error: Error::new(ErrorKind::Abort, "The upgrade transaction was aborted"),
            },
        ));
        // NB: The agent queues the open request's error event once its upgrade transaction's abort steps ran, and the
        //     connection queue's next request waits for that.
        if let Some(entry) = self.queues.get_mut(key).and_then(|queue| queue.front_mut()) {
            entry.stage = Stage::WaitingForOpenHandled;
        }
    }

    fn open_handled(&mut self, client: ClientId, request: RequestId) {
        let keys: Vec<_> = self.queues.keys().cloned().collect();
        for key in keys {
            let is_head = self.queues.get(&key).and_then(|queue| queue.front()).is_some_and(|entry| {
                matches!(entry.stage, Stage::WaitingForOpenHandled)
                    && matches!(entry.pending, Pending::Open { client: owner, request: id, .. } if owner == client && id == request)
            });
            if is_head {
                if let Some(queue) = self.queues.get_mut(&key) {
                    queue.pop_front();
                }
                self.advance_queue(&key);
                return;
            }
        }
    }

    // https://w3c.github.io/IndexedDB/#close-a-database-connection
    fn request_close(&mut self, connection: ConnectionId) {
        // 1. Set connection's close pending flag to true.
        let Some(entry) = self.connections.get_mut(&connection) else {
            return;
        };
        entry.close_pending = true;
        self.maybe_finish_close(connection);
    }

    /// Closes a connection whose close is pending once all its transactions have finished.
    fn maybe_finish_close(&mut self, connection: ConnectionId) {
        // 3. Wait for all transactions created using connection to complete. Once they are complete, connection is
        //    closed.
        if self
            .transactions
            .values()
            .any(|transaction| transaction.connection == connection)
        {
            return;
        }
        let Some(entry) = self.connections.get(&connection) else {
            return;
        };
        if !entry.close_pending {
            return;
        }
        self.close_connection(connection);
    }

    fn close_connection(&mut self, connection: ConnectionId) {
        self.stale_acknowledgements.remove(&connection);
        let Some(entry) = self.connections.remove(&connection) else {
            return;
        };
        let key = (entry.storage_key, entry.database);
        if let Some(database) = self.database_mut(&key.0, &key.1) {
            database.connections.retain(|other| *other != connection);
        }
        self.advance_queue(&key);
    }

    fn create_transaction(
        &mut self,
        client: ClientId,
        connection: ConnectionId,
        transaction: TransactionId,
        scope: Vec<Name>,
        mode: Mode,
    ) {
        let Some(entry) = self.connections.get(&connection) else {
            return;
        };
        if entry.client != client || mode == Mode::VersionChange {
            return;
        }
        // NB: The agent numbers its transactions once each, below the numbers the backend gives upgrade transactions;
        //     one that reuses a number misbehaved, and its transaction never exists.
        if transaction >= UPGRADE_TRANSACTION_ID_BASE || self.transactions.contains_key(&(client, transaction)) {
            return;
        }
        let (storage_key, database) = (entry.storage_key.clone(), entry.database.clone());
        self.transactions.insert(
            (client, transaction),
            Transaction {
                connection,
                storage_key: storage_key.clone(),
                database: database.clone(),
                scope,
                mode,
                state: TransactionState::Waiting,
                operations: VecDeque::new(),
                commit_requested: false,
                undone: BTreeSet::new(),
                retained: 0,
                undo: Vec::new(),
                snapshot: None,
                cursors: HashMap::new(),
            },
        );
        if let Some(entry) = self.database_mut(&storage_key, &database) {
            entry.transactions.push((client, transaction));
        }
        self.schedule(&storage_key, &database);
    }

    /// Starts the waiting transactions of a database that can start now. A transaction that finishes while they run asks
    /// for its database to be scheduled again, which this loop then does, rather than a call deeper in the stack: A
    /// chain of transactions that each wait for the one before it would otherwise nest a call for each of them.
    fn schedule(&mut self, storage_key: &str, name: &Name) {
        self.to_schedule.push_back((storage_key.to_owned(), name.clone()));
        if self.scheduling {
            return;
        }
        self.scheduling = true;
        while let Some((storage_key, name)) = self.to_schedule.pop_front() {
            self.start_waiting_transactions(&storage_key, &name);
        }
        self.scheduling = false;
    }

    // https://w3c.github.io/IndexedDB/#transaction-scheduling
    fn start_waiting_transactions(&mut self, storage_key: &str, name: &Name) {
        let Some(database) = self.database(storage_key, name) else {
            return;
        };
        let order = database.transactions.clone();
        let mut to_start = Vec::new();
        for (position, id) in order.iter().enumerate() {
            let Some(transaction) = self.transactions.get(id) else {
                continue;
            };
            if transaction.state != TransactionState::Waiting {
                continue;
            }
            let overlaps = |other: &Transaction| other.scope.iter().any(|store| transaction.scope.contains(store));
            let blocked = order[..position].iter().any(|earlier| {
                let Some(earlier) = self.transactions.get(earlier) else {
                    return false;
                };
                // * A read-only transaction can start when there are no read/write transactions which were created
                //   before tx and have overlapping scopes with tx and are not finished.
                // * A read/write transaction can start when there are no transactions which were created before tx
                //   and have overlapping scopes with tx and are not finished.
                overlaps(earlier) && (transaction.mode == Mode::ReadWrite || earlier.mode != Mode::ReadOnly)
            });
            if !blocked {
                to_start.push(*id);
            }
        }
        for id in to_start {
            if let Some(transaction) = self.transactions.get_mut(&id) {
                transaction.state = TransactionState::Running;
            }
            self.run_transaction(id.0, id.1);
        }
    }

    /// Performs the operations of a running transaction that the agent sent, and commits it once asked to and done.
    fn run_transaction(&mut self, client: ClientId, id: TransactionId) {
        loop {
            let Some(transaction) = self.transactions.get_mut(&(client, id)) else {
                return;
            };
            if transaction.state != TransactionState::Running {
                return;
            }
            let Some((request, operation)) = transaction.operations.pop_front() else {
                if transaction.commit_requested {
                    self.commit_transaction(client, id);
                }
                return;
            };
            let event = match self.perform(client, id, operation) {
                Ok(result) => Event::OperationSucceeded {
                    transaction: id,
                    request,
                    result,
                },
                Err(error) => Event::OperationFailed {
                    transaction: id,
                    request,
                    error,
                },
            };
            self.events.push((client, event));
        }
    }

    // https://w3c.github.io/IndexedDB/#commit-a-transaction
    fn commit_transaction(&mut self, client: ClientId, id: TransactionId) {
        let transaction = self.transactions.remove(&(client, id)).expect("the transaction exists");

        // 4. If an error occurs while writing the changes to the database, then run abort a transaction with
        //    transaction and an appropriate type for the error, for example a "QuotaExceededError" or an
        //    "UnknownError" DOMException, and terminate these steps.
        if !self.persist_commit(&transaction) {
            self.transactions.insert((client, id), transaction);
            self.abort_transaction(client, id);
            return;
        }
        // NB: Only a transaction that changed something marks its storage key as written.
        if transaction.snapshot.is_some() || !transaction.undo.is_empty() {
            self.written.insert(transaction.storage_key.clone(), now());
        }
        self.events
            .push((client, Event::TransactionCompleted { transaction: id }));
        self.finish_transaction(client, id, transaction, true);
    }

    // https://w3c.github.io/IndexedDB/#abort-a-transaction
    fn abort_transaction(&mut self, client: ClientId, id: TransactionId) {
        let Some(mut transaction) = self.transactions.remove(&(client, id)) else {
            return;
        };

        // 1. All the changes made to the database by the transaction are reverted. For upgrade transactions this
        //    includes changes to the set of object stores and indexes, as well as the change to the version.
        if let Some(database) = self.database_mut(&transaction.storage_key, &transaction.database) {
            if let Some(snapshot) = transaction.snapshot.take() {
                database.version = snapshot.version;
                database.object_stores = snapshot.object_stores;
            } else {
                for undo in transaction.undo.drain(..).rev() {
                    match undo {
                        Undo::Record {
                            store,
                            key,
                            previous,
                            previous_index_records,
                            ..
                        } => {
                            if let Ok(store) = database.store_mut(&store) {
                                store.restore_record(key, previous, previous_index_records);
                            }
                        }
                        Undo::KeyGenerator { store, current_number } => {
                            if let Ok(store) = database.store_mut(&store)
                                && let Some(generator) = store.key_generator.as_mut()
                            {
                                generator.set(current_number);
                            }
                        }
                    }
                }
            }
        }

        // 5. For each request of transaction's request list, the request's done flag is set and the abort error is
        //    fired at it by the agent, which knows its requests.
        self.events
            .push((client, Event::TransactionAborted { transaction: id }));
        self.finish_transaction(client, id, transaction, false);
    }

    fn finish_transaction(&mut self, client: ClientId, id: TransactionId, transaction: Transaction, committed: bool) {
        let key = (transaction.storage_key.clone(), transaction.database.clone());
        if let Some(database) = self.database_mut(&key.0, &key.1) {
            database.transactions.retain(|entry| *entry != (client, id));
        }
        if transaction.mode == Mode::VersionChange {
            // A database that its first upgrade created is gone again when that upgrade aborts.
            if !committed
                && let Some(database) = self.database(&key.0, &key.1)
                && database.version == 0
            {
                let connections = database.connections.clone();
                for connection in connections {
                    self.connections.remove(&connection);
                }
                self.forget_database(&key.0, &key.1);
            }
            self.finish_upgrade(&key, committed);
        }
        self.maybe_finish_close(transaction.connection);
        self.schedule(&key.0, &key.1);
    }

    fn perform(&mut self, client: ClientId, id: TransactionId, operation: Operation) -> Result<OperationResult, Error> {
        let transaction = self
            .transactions
            .get_mut(&(client, id))
            .expect("the transaction exists");
        let (storage_key, name, mode) = (
            transaction.storage_key.clone(),
            transaction.database.clone(),
            transaction.mode,
        );
        // NB: The agent only makes requests against the object stores in its transaction's scope, which scheduling
        //     relies on; one that asks for another store misbehaved. An upgrade transaction's scope is every store.
        if mode != Mode::VersionChange
            && let Some(store) = operation.store()
            && !transaction.scope.contains(store)
        {
            return Err(Error::new(
                ErrorKind::NotFound,
                "The object store is not in the transaction's scope",
            ));
        }
        self.check_quota(client, id, &storage_key, &name, &operation)?;
        let transaction = self
            .transactions
            .get_mut(&(client, id))
            .expect("the transaction exists");
        let mut undo = core::mem::take(&mut transaction.undo);
        let mut cursors = core::mem::take(&mut transaction.cursors);
        let database = self
            .databases
            .get_mut(&storage_key)
            .and_then(|databases| databases.iter_mut().find(|database| database.name == name))
            .ok_or_else(|| Error::new(ErrorKind::Unknown, "The database is gone"));
        let undo_before = undo.len();
        // NB: An upgrade's snapshot keeps what a store or an index that it deletes held before the upgrade, until the
        //     upgrade ends; one that the upgrade created leaves nothing behind.
        let snapshot_has = |store_id: u64, index_id: Option<u64>| {
            transaction.snapshot.as_ref().is_some_and(|snapshot| {
                snapshot.object_stores.iter().any(|store| {
                    store.id == store_id && index_id.is_none_or(|id| store.indexes.iter().any(|index| index.id == id))
                })
            })
        };
        let kept_by_snapshot = match (&database, &operation) {
            (Ok(database), Operation::DeleteObjectStore { name }) => database
                .store(name)
                .ok()
                .filter(|store| snapshot_has(store.id, None))
                .map_or(0, ObjectStore::bytes),
            (Ok(database), Operation::DeleteIndex { store, name }) => database
                .store(store)
                .ok()
                .and_then(|store| store.index(name).filter(|index| snapshot_has(store.id, Some(index.id))))
                .map_or(0, Index::bytes),
            _ => 0,
        };
        let result = database.and_then(|database| Self::perform_on(database, mode, &mut undo, &mut cursors, operation));
        let transaction = self
            .transactions
            .get_mut(&(client, id))
            .expect("the transaction exists");
        for entry in undo.split_off(undo_before) {
            if let Undo::Record {
                store_id,
                key,
                previous,
                previous_index_records,
                ..
            } = &entry
            {
                if !transaction.undone.insert((*store_id, key.clone())) {
                    continue;
                }
                // NB: An abort puts back the record's index records too.
                let index_bytes: u64 = previous_index_records
                    .iter()
                    .flat_map(|(_, index_keys)| index_keys)
                    .map(|index_key| key_bytes(index_key.key()) + key_bytes(key.key()))
                    .sum();
                transaction.retained += previous
                    .as_ref()
                    .map_or(0, |value| key_bytes(key.key()) + value.len() as u64 + index_bytes);
            }
            undo.push(entry);
        }
        if result.is_ok() {
            transaction.retained += kept_by_snapshot;
        }
        transaction.undo = undo;
        transaction.cursors = cursors;
        result
    }

    /// Fails a put, or a new index's records, that would take its storage key's databases, or all of them, past their
    /// limit. Replacing a record gives back what that record took, except to the session's limit the first time a
    /// transaction replaces it, when its undo entry keeps the old value until the transaction ends.
    fn check_quota(
        &self,
        client: ClientId,
        id: TransactionId,
        storage_key: &str,
        name: &Name,
        operation: &Operation,
    ) -> Result<(), Error> {
        let (Operation::Put { store: store_name, .. } | Operation::AddIndexKeys { store: store_name, .. }) = operation
        else {
            return Ok(());
        };
        let Some(store) = self
            .databases
            .get(storage_key)
            .and_then(|databases| databases.iter().find(|database| &database.name == name))
            .and_then(|database| database.store(store_name).ok())
        else {
            return Ok(());
        };
        let (added, replaced) = match operation {
            Operation::Put {
                value, key, index_keys, ..
            } => {
                // NB: A key the backend generates is a number.
                let key_size = key.as_ref().map_or(8, key_bytes);
                let index_bytes: u64 = index_keys
                    .iter()
                    .map(|entry| match &entry.keys {
                        IndexKeys::Keys(keys) => keys
                            .iter()
                            .map(|index_key| key_bytes(index_key) + key_size)
                            .sum::<u64>(),
                    })
                    .sum();
                let replaced = key
                    .as_ref()
                    .map_or(0, |key| store.bytes_of_record(&OrderedKey(Rc::new(key.clone()))));
                (key_size + value.len() as u64 + index_bytes, replaced)
            }
            Operation::AddIndexKeys { entries, .. } => {
                let added = entries
                    .iter()
                    .flat_map(|entry| {
                        entry
                            .keys
                            .iter()
                            .map(|index_key| key_bytes(index_key) + key_bytes(&entry.primary_key))
                    })
                    .sum();
                (added, 0)
            }
            _ => return Ok(()),
        };
        let bytes_of = |databases: &Vec<Database>| -> u64 {
            databases
                .iter()
                .flat_map(|database| &database.object_stores)
                .map(ObjectStore::bytes)
                .sum()
        };
        // NB: What other transactions on the storage key keep for an abort comes back if they abort, so it still counts
        //     against the quota; what this transaction keeps doesn't, as replacing a record is no growth.
        let restorable: u64 = self
            .transactions
            .iter()
            .filter(|(key, transaction)| **key != (client, id) && transaction.storage_key == storage_key)
            .map(|(_, transaction)| transaction.retained)
            .sum();
        let of_storage_key = self.databases.get(storage_key).map_or(0, bytes_of) + restorable;
        let retained: u64 = self.transactions.values().map(|transaction| transaction.retained).sum();
        let of_session = self.databases.values().map(bytes_of).sum::<u64>() + retained;
        let replaced_already_undone = match operation {
            Operation::Put { key: Some(key), .. } => self.transactions.get(&(client, id)).is_some_and(|transaction| {
                transaction
                    .undone
                    .contains(&(store.id, OrderedKey(Rc::new(key.clone()))))
            }),
            _ => false,
        };
        let replaced_for_session = if replaced_already_undone { replaced } else { 0 };
        if (of_storage_key + added).saturating_sub(replaced) > self.limits.storage_key
            || (of_session + added).saturating_sub(replaced_for_session) > self.limits.session
        {
            return Err(Error::new(
                ErrorKind::QuotaExceeded,
                "The databases of this storage key are at their quota",
            ));
        }
        Ok(())
    }

    fn perform_on(
        database: &mut Database,
        mode: Mode,
        undo: &mut Vec<Undo>,
        cursors: &mut HashMap<CursorId, Cursor>,
        operation: Operation,
    ) -> Result<OperationResult, Error> {
        let require_write = || {
            if mode == Mode::ReadOnly {
                Err(Error::new(ErrorKind::Unknown, "The transaction is read-only"))
            } else {
                Ok(())
            }
        };
        let require_upgrade = || {
            if mode == Mode::VersionChange {
                Ok(())
            } else {
                Err(Error::new(
                    ErrorKind::InvalidState,
                    "The transaction is not an upgrade transaction",
                ))
            }
        };
        let first = |database: &Database, source: &Source, range: &KeyRange| -> Result<Option<FoundRecord>, Error> {
            let store = database.store(source.store_name())?;
            Ok(records_from(store, source, range, false)?
                .next()
                .map(|(key, referenced)| FoundRecord {
                    key: key.key().as_ref().clone(),
                    primary_key: referenced.key().as_ref().clone(),
                    value: store.records.get(referenced).map(|value| value.as_ref().clone()),
                }))
        };

        match operation {
            Operation::Put {
                store,
                value,
                key,
                index_keys,
                no_overwrite,
            } => {
                require_write()?;
                let store = database.store_mut(&store)?;
                Ok(OperationResult::Key(store.store_a_record(
                    Rc::new(value),
                    key,
                    &index_keys,
                    no_overwrite,
                    undo,
                )?))
            }
            // https://w3c.github.io/IndexedDB/#retrieve-a-value-from-an-object-store
            // https://w3c.github.io/IndexedDB/#retrieve-a-referenced-value-from-an-index
            Operation::Get { source, range } => Ok(OperationResult::Value(first(database, &source, &range)?)),
            // https://w3c.github.io/IndexedDB/#retrieve-a-key-from-an-object-store
            // https://w3c.github.io/IndexedDB/#retrieve-a-value-from-an-index
            Operation::GetKey { source, range } => Ok(OperationResult::OptionalKey(
                first(database, &source, &range)?.map(|record| record.primary_key),
            )),
            // https://w3c.github.io/IndexedDB/#retrieve-multiple-items-from-an-object-store
            // https://w3c.github.io/IndexedDB/#retrieve-multiple-items-from-an-index
            Operation::GetAll {
                source,
                range,
                count,
                direction,
                retrieve,
            } => {
                let store = database.store(source.store_name())?;
                let reverse = matches!(direction, Direction::Prev | Direction::PrevUnique);
                let unique = matches!(direction, Direction::NextUnique | Direction::PrevUnique);
                let limit = count
                    .filter(|count| *count > 0)
                    .map_or(usize::MAX, |count| count as usize);
                let mut records: Vec<FoundRecord> = Vec::new();
                let mut previous_key: Option<OrderedKey> = None;
                for (key, referenced) in records_from(store, &source, &range, reverse)? {
                    // NB: The limit applies once a new key comes, so prevunique still finishes the last key's records.
                    let new_key = !unique || previous_key.as_ref() != Some(key);
                    if new_key && records.len() >= limit {
                        break;
                    }
                    if unique {
                        if previous_key.as_ref() == Some(key) {
                            // prevunique reports the first record of each key, which a reverse walk meets last.
                            if reverse {
                                let last = records.last_mut().expect("a record was found for the key");
                                last.primary_key = referenced.key().as_ref().clone();
                                if retrieve != Retrieve::Keys {
                                    last.value = store.records.get(referenced).map(|value| value.as_ref().clone());
                                }
                            }
                            continue;
                        }
                        previous_key = Some(key.clone());
                    }
                    records.push(FoundRecord {
                        key: key.key().as_ref().clone(),
                        primary_key: referenced.key().as_ref().clone(),
                        value: if retrieve == Retrieve::Keys {
                            None
                        } else {
                            store.records.get(referenced).map(|value| value.as_ref().clone())
                        },
                    });
                }
                Ok(OperationResult::Records(records))
            }
            // https://w3c.github.io/IndexedDB/#count-the-records-in-a-range
            Operation::Count { source, range } => {
                let store = database.store(source.store_name())?;
                Ok(OperationResult::Count(
                    records_from(store, &source, &range, false)?.count() as u64,
                ))
            }
            // https://w3c.github.io/IndexedDB/#delete-records-from-an-object-store
            Operation::Delete { store, range } => {
                require_write()?;
                database.store_mut(&store)?.delete_records(&range, undo);
                Ok(OperationResult::Undefined)
            }
            // https://w3c.github.io/IndexedDB/#clear-an-object-store
            Operation::Clear { store } => {
                require_write()?;
                database.store_mut(&store)?.clear(undo);
                Ok(OperationResult::Undefined)
            }
            Operation::OpenCursor {
                cursor,
                source,
                range,
                direction,
                key_only,
            } => {
                let store = database.store(source.store_name())?;
                let mut new_cursor = Cursor::new(source, range, direction, key_only);
                let found = new_cursor.iterate(store, None, None, 1)?;
                cursors.insert(cursor, new_cursor);
                Ok(OperationResult::Cursor(found))
            }
            Operation::IterateCursor {
                cursor,
                key,
                primary_key,
                count,
            } => {
                let cursor = cursors
                    .get_mut(&cursor)
                    .ok_or_else(|| Error::new(ErrorKind::InvalidState, "No cursor with this identifier"))?;
                let store = database.store(cursor.source.store_name())?;
                Ok(OperationResult::Cursor(cursor.iterate(
                    store,
                    key,
                    primary_key,
                    count,
                )?))
            }
            // https://w3c.github.io/IndexedDB/#dom-idbdatabase-createobjectstore
            Operation::CreateObjectStore {
                name,
                key_path,
                auto_increment,
            } => {
                require_upgrade()?;
                if database.store(&name).is_ok() {
                    return Err(Error::new(
                        ErrorKind::Constraint,
                        "An object store with this name already exists",
                    ));
                }
                let id = database.take_id();
                database
                    .object_stores
                    .push(ObjectStore::new(id, name, key_path, auto_increment));
                Ok(OperationResult::Undefined)
            }
            // https://w3c.github.io/IndexedDB/#dom-idbdatabase-deleteobjectstore
            Operation::DeleteObjectStore { name } => {
                require_upgrade()?;
                database.store(&name)?;
                database.object_stores.retain(|store| store.name != name);
                Ok(OperationResult::Undefined)
            }
            // https://w3c.github.io/IndexedDB/#dom-idbobjectstore-name
            Operation::RenameObjectStore { name, new_name } => {
                require_upgrade()?;
                if database.store(&new_name).is_ok() {
                    return Err(Error::new(
                        ErrorKind::Constraint,
                        "An object store with this name already exists",
                    ));
                }
                database.store_mut(&name)?.name = new_name.clone();
                // NB: A cursor that the transaction opened keeps reading the object store under its new name.
                for cursor in cursors.values_mut() {
                    match &mut cursor.source {
                        Source::ObjectStore(store) | Source::Index { store, .. } if *store == name => {
                            *store = new_name.clone();
                        }
                        _ => {}
                    }
                }
                Ok(OperationResult::Undefined)
            }
            // https://w3c.github.io/IndexedDB/#dom-idbobjectstore-createindex
            Operation::CreateIndex {
                store,
                name,
                key_path,
                unique,
                multi_entry,
            } => {
                require_upgrade()?;
                let id = database.take_id();
                let store = database.store_mut(&store)?;
                if store.index(&name).is_some() {
                    return Err(Error::new(
                        ErrorKind::Constraint,
                        "An index with this name already exists",
                    ));
                }
                store.indexes.push(Index::new(id, name, key_path, unique, multi_entry));
                Ok(OperationResult::Undefined)
            }
            // https://w3c.github.io/IndexedDB/#dom-idbobjectstore-deleteindex
            Operation::DeleteIndex { store, name } => {
                require_upgrade()?;
                let store = database.store_mut(&store)?;
                if store.index(&name).is_none() {
                    return Err(Error::new(ErrorKind::NotFound, "No index with this name"));
                }
                store.indexes.retain(|index| index.name != name);
                Ok(OperationResult::Undefined)
            }
            // https://w3c.github.io/IndexedDB/#dom-idbindex-name
            Operation::RenameIndex {
                store: store_name,
                name,
                new_name,
            } => {
                require_upgrade()?;
                let store = database.store_mut(&store_name)?;
                if store.index(&new_name).is_some() {
                    return Err(Error::new(
                        ErrorKind::Constraint,
                        "An index with this name already exists",
                    ));
                }
                store
                    .index_mut(&name)
                    .ok_or_else(|| Error::new(ErrorKind::NotFound, "No index with this name"))?
                    .name = new_name.clone();
                store.rename_population(&name, &new_name);
                // NB: A cursor that the transaction opened keeps reading the index under its new name.
                for cursor in cursors.values_mut() {
                    if let Source::Index { store, index } = &mut cursor.source
                        && *store == store_name
                        && *index == name
                    {
                        *index = new_name.clone();
                    }
                }
                Ok(OperationResult::Undefined)
            }
            Operation::PopulateIndex { store, index } => {
                require_upgrade()?;
                let store = database.store_mut(&store)?;
                store.begin_population(&index);
                Ok(OperationResult::Records(
                    store
                        .records
                        .iter()
                        .map(|(key, value)| FoundRecord {
                            key: key.key().as_ref().clone(),
                            primary_key: key.key().as_ref().clone(),
                            value: Some(value.as_ref().clone()),
                        })
                        .collect(),
                ))
            }
            Operation::AddIndexKeys { store, index, entries } => {
                require_upgrade()?;
                let store = database.store_mut(&store)?;
                let entries: Vec<(Key, Vec<Key>)> = entries
                    .into_iter()
                    .map(|entry| (entry.primary_key, entry.keys))
                    .collect();
                store.finish_population(&index, &entries)?;
                Ok(OperationResult::Undefined)
            }
        }
    }
}
