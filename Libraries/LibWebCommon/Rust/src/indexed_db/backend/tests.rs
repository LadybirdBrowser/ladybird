/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use crate::indexed_db::backend::store::{self, IndexKeys};

const STORAGE_KEY: &str = "origin";

fn name(value: &str) -> Name {
    Name::from(value)
}

fn number(value: f64) -> Key {
    Key::create_number(value).as_ref().clone()
}

fn string(value: &str) -> Key {
    Key::create_string(ak::Utf16String::from_utf8(value)).as_ref().clone()
}

fn value(bytes: &[u8]) -> Vec<u8> {
    bytes.to_vec()
}

fn numbers(records: &[FoundRecord]) -> Vec<f64> {
    records
        .iter()
        .map(|record| record.primary_key.value_as_double())
        .collect()
}

struct Harness {
    backend: Backend,
    next_request: RequestId,
    next_client: ClientId,
}

impl Harness {
    fn new() -> Self {
        Self::with_backend(Backend::new())
    }

    fn with_backend(backend: Backend) -> Self {
        Self {
            backend,
            next_request: 0,
            next_client: 0,
        }
    }

    fn client(&mut self) -> ClientId {
        self.client_of(STORAGE_KEY)
    }

    fn client_of(&mut self, storage_key: &str) -> ClientId {
        self.next_client += 1;
        self.backend.connect_client(self.next_client, storage_key.to_owned());
        self.next_client
    }

    fn request(&mut self) -> RequestId {
        self.next_request += 1;
        self.next_request
    }

    fn events(&mut self) -> Vec<(ClientId, Event)> {
        self.backend.take_events()
    }

    /// Opens a database at a version, and returns the connection and, if it upgraded, the upgrade transaction.
    fn open(
        &mut self,
        client: ClientId,
        database: &str,
        version: Option<u64>,
    ) -> (ConnectionId, Option<TransactionId>) {
        let request = self.request();
        self.backend.handle(
            client,
            Request::Open {
                request,
                name: name(database),
                version,
            },
        );
        for (target, event) in self.events() {
            assert_eq!(target, client);
            match event {
                Event::UpgradeNeeded {
                    connection,
                    transaction,
                    ..
                } => return (connection, Some(transaction)),
                Event::OpenSucceeded { connection, .. } => {
                    self.backend.handle(client, Request::OpenHandled { request });
                    return (connection, None);
                }
                _ => {}
            }
        }
        panic!("the open request neither upgraded nor succeeded");
    }

    fn operation(
        &mut self,
        client: ClientId,
        transaction: TransactionId,
        operation: Operation,
    ) -> Result<OperationResult, Error> {
        let request = self.request();
        self.backend.handle(
            client,
            Request::Operation {
                transaction,
                request,
                operation,
            },
        );
        for (_, event) in self.events() {
            match event {
                Event::OperationSucceeded {
                    request: id, result, ..
                } if id == request => return Ok(result),
                Event::OperationFailed { request: id, error, .. } if id == request => return Err(error),
                _ => {}
            }
        }
        panic!("the operation got no answer");
    }

    fn commit(&mut self, client: ClientId, transaction: TransactionId) -> Vec<(ClientId, Event)> {
        self.backend.handle(client, Request::Commit { transaction });
        let events = self.events();
        // An upgrade's open request succeeds once its transaction commits, and the agent acknowledges that.
        for (target, event) in &events {
            if let Event::OpenSucceeded { request, .. } = event {
                self.backend.handle(*target, Request::OpenHandled { request: *request });
            }
        }
        events
    }

    fn create_store(&mut self, client: ClientId, transaction: TransactionId, store: &str, auto_increment: bool) {
        self.operation(
            client,
            transaction,
            Operation::CreateObjectStore {
                name: name(store),
                key_path: None,
                auto_increment,
            },
        )
        .unwrap();
    }

    fn put(
        &mut self,
        client: ClientId,
        transaction: TransactionId,
        store: &str,
        key: Option<Key>,
        bytes: &[u8],
    ) -> Result<Key, Error> {
        match self.operation(
            client,
            transaction,
            Operation::Put {
                store: name(store),
                value: value(bytes),
                key,
                index_keys: Vec::new(),
                no_overwrite: false,
            },
        )? {
            OperationResult::Key(key) => Ok(key),
            _ => panic!("a put returns a key"),
        }
    }

    fn get(&mut self, client: ClientId, transaction: TransactionId, store: &str, key: Key) -> Option<Vec<u8>> {
        match self
            .operation(
                client,
                transaction,
                Operation::Get {
                    source: Source::ObjectStore(name(store)),
                    range: KeyRange::only(key),
                },
            )
            .unwrap()
        {
            OperationResult::Value(record) => record.map(|record| record.value.unwrap()),
            _ => panic!("a get returns a value"),
        }
    }

    fn transaction(
        &mut self,
        client: ClientId,
        connection: ConnectionId,
        id: TransactionId,
        scope: &[&str],
        mode: Mode,
    ) {
        self.backend.handle(
            client,
            Request::CreateTransaction {
                connection,
                transaction: id,
                scope: scope.iter().map(|store| name(store)).collect(),
                mode,
            },
        );
    }

    /// Creates a database with one object store holding the given records.
    fn database_with_records(&mut self, client: ClientId, records: &[(f64, &[u8])]) -> ConnectionId {
        let (connection, upgrade) = self.open(client, "db", Some(1));
        let upgrade = upgrade.unwrap();
        self.create_store(client, upgrade, "store", false);
        for (key, bytes) in records {
            self.put(client, upgrade, "store", Some(number(*key)), bytes).unwrap();
        }
        self.commit(client, upgrade);
        connection
    }
}

#[test]
fn a_second_client_sees_what_the_first_one_stored() {
    let mut harness = Harness::new();
    let first = harness.client();
    let connection = harness.database_with_records(first, &[(1.0, b"one")]);
    harness.backend.handle(first, Request::Close { connection });

    // The agent that reads is another one: another process, or a worker of the same page.
    let second = harness.client();
    let (connection, upgrade) = harness.open(second, "db", None);
    assert!(upgrade.is_none());
    harness.transaction(second, connection, 1, &["store"], Mode::ReadOnly);
    assert_eq!(harness.get(second, 1, "store", number(1.0)), Some(b"one".to_vec()));
}

#[test]
fn a_storage_key_does_not_see_another_ones_databases() {
    let mut harness = Harness::new();
    let first = harness.client();
    harness.database_with_records(first, &[(1.0, b"one")]);

    let other = harness.client_of("other");
    let (_, upgrade) = harness.open(other, "db", None);
    assert!(upgrade.is_some(), "the other storage key gets a new database");
}

#[test]
fn opening_with_a_lower_version_fails() {
    let mut harness = Harness::new();
    let client = harness.client();
    let (_, upgrade) = harness.open(client, "db", Some(2));
    harness.commit(client, upgrade.unwrap());

    let request = harness.request();
    harness.backend.handle(
        client,
        Request::Open {
            request,
            name: name("db"),
            version: Some(1),
        },
    );
    let events = harness.events();
    assert!(matches!(
        &events[..],
        [(_, Event::OpenFailed { error, .. })] if error.kind == ErrorKind::Version
    ));
}

#[test]
fn an_aborted_first_upgrade_leaves_no_database() {
    let mut harness = Harness::new();
    let client = harness.client();
    let (_, upgrade) = harness.open(client, "db", Some(1));
    harness.backend.handle(
        client,
        Request::Abort {
            transaction: upgrade.unwrap(),
        },
    );
    let events = harness.events();
    assert!(
        events
            .iter()
            .any(|(_, event)| matches!(event, Event::TransactionAborted { .. }))
    );
    let Some(open) = events.iter().find_map(|(_, event)| match event {
        Event::OpenFailed { request, error } if error.kind == ErrorKind::Abort => Some(*request),
        _ => None,
    }) else {
        panic!("the open request fails");
    };
    harness.backend.handle(client, Request::OpenHandled { request: open });

    let request = harness.request();
    harness.backend.handle(client, Request::Databases { request });
    assert!(matches!(&harness.events()[..], [(_, Event::DatabasesListed { databases, .. })] if databases.is_empty()));
}

#[test]
fn an_aborted_upgrade_puts_the_version_and_the_stores_back() {
    let mut harness = Harness::new();
    let client = harness.client();
    let connection = harness.database_with_records(client, &[(1.0, b"one")]);
    harness.backend.handle(client, Request::Close { connection });

    let (_, upgrade) = harness.open(client, "db", Some(2));
    let upgrade = upgrade.unwrap();
    harness
        .operation(client, upgrade, Operation::DeleteObjectStore { name: name("store") })
        .unwrap();
    harness.backend.handle(client, Request::Abort { transaction: upgrade });
    for (_, event) in harness.events() {
        if let Event::OpenFailed { request, .. } = event {
            harness.backend.handle(client, Request::OpenHandled { request });
        }
    }

    // Version 1 opens without an upgrade only if the abort put it back.
    let (connection, upgrade) = harness.open(client, "db", Some(1));
    assert!(upgrade.is_none());
    harness.transaction(client, connection, 1, &["store"], Mode::ReadOnly);
    assert_eq!(harness.get(client, 1, "store", number(1.0)), Some(b"one".to_vec()));
}

#[test]
fn an_upgrade_waits_for_other_connections_and_reports_blocked() {
    let mut harness = Harness::new();
    let first = harness.client();
    let old_connection = harness.database_with_records(first, &[]);

    let second = harness.client();
    let request = harness.request();
    harness.backend.handle(
        second,
        Request::Open {
            request,
            name: name("db"),
            version: Some(2),
        },
    );
    let events = harness.events();
    assert!(matches!(
        &events[..],
        [(target, Event::VersionChange { old_version: 1, new_version: Some(2), .. })] if *target == first
    ));

    // The first agent fired versionchange, and didn't close its connection in it.
    harness.backend.handle(
        first,
        Request::VersionChangeHandled {
            connection: old_connection,
        },
    );
    let events = harness.events();
    assert!(matches!(
        &events[..],
        [(target, Event::Blocked { old_version: 1, new_version: Some(2), .. })] if *target == second
    ));

    // Closing it lets the upgrade go on, and blocked isn't reported again.
    harness.backend.handle(
        first,
        Request::Close {
            connection: old_connection,
        },
    );
    let events = harness.events();
    assert!(matches!(&events[..], [(target, Event::UpgradeNeeded { old_version: 1, .. })] if *target == second));
}

#[test]
fn deleting_a_database_waits_for_its_connections() {
    let mut harness = Harness::new();
    let client = harness.client();
    let connection = harness.database_with_records(client, &[(1.0, b"one")]);

    let request = harness.request();
    harness.backend.handle(
        client,
        Request::DeleteDatabase {
            request,
            name: name("db"),
        },
    );
    assert!(matches!(
        &harness.events()[..],
        [(_, Event::VersionChange { new_version: None, .. })]
    ));
    harness.backend.handle(client, Request::Close { connection });
    harness
        .backend
        .handle(client, Request::VersionChangeHandled { connection });
    assert!(matches!(
        &harness.events()[..],
        [(_, Event::DeleteSucceeded { old_version: 1, .. })]
    ));

    let (_, upgrade) = harness.open(client, "db", None);
    assert!(upgrade.is_some(), "the database is gone");
}

#[test]
fn an_aborted_transaction_puts_records_back() {
    let mut harness = Harness::new();
    let client = harness.client();
    let connection = harness.database_with_records(client, &[(1.0, b"one"), (2.0, b"two")]);

    harness.transaction(client, connection, 1, &["store"], Mode::ReadWrite);
    harness.put(client, 1, "store", Some(number(1.0)), b"changed").unwrap();
    harness.put(client, 1, "store", Some(number(3.0)), b"three").unwrap();
    harness
        .operation(
            client,
            1,
            Operation::Delete {
                store: name("store"),
                range: KeyRange::only(number(2.0)),
            },
        )
        .unwrap();
    harness.backend.handle(client, Request::Abort { transaction: 1 });
    harness.events();

    harness.transaction(client, connection, 2, &["store"], Mode::ReadOnly);
    assert_eq!(harness.get(client, 2, "store", number(1.0)), Some(b"one".to_vec()));
    assert_eq!(harness.get(client, 2, "store", number(2.0)), Some(b"two".to_vec()));
    assert_eq!(harness.get(client, 2, "store", number(3.0)), None);
}

#[test]
fn a_key_generator_counts_on_and_reverts_with_an_abort() {
    let mut harness = Harness::new();
    let client = harness.client();
    let (connection, upgrade) = harness.open(client, "db", Some(1));
    let upgrade = upgrade.unwrap();
    harness.create_store(client, upgrade, "store", true);
    assert_eq!(
        harness
            .put(client, upgrade, "store", None, b"a")
            .unwrap()
            .value_as_double(),
        1.0
    );
    assert_eq!(
        harness
            .put(client, upgrade, "store", Some(number(10.5)), b"b")
            .unwrap()
            .value_as_double(),
        10.5
    );
    assert_eq!(
        harness
            .put(client, upgrade, "store", None, b"c")
            .unwrap()
            .value_as_double(),
        11.0
    );
    harness.commit(client, upgrade);

    harness.transaction(client, connection, 1, &["store"], Mode::ReadWrite);
    assert_eq!(
        harness.put(client, 1, "store", None, b"d").unwrap().value_as_double(),
        12.0
    );
    harness.backend.handle(client, Request::Abort { transaction: 1 });
    harness.events();

    harness.transaction(client, connection, 2, &["store"], Mode::ReadWrite);
    assert_eq!(
        harness.put(client, 2, "store", None, b"e").unwrap().value_as_double(),
        12.0
    );
}

#[test]
fn a_unique_index_refuses_a_second_record_with_its_key() {
    let mut harness = Harness::new();
    let client = harness.client();
    let (_, upgrade) = harness.open(client, "db", Some(1));
    let upgrade = upgrade.unwrap();
    harness.create_store(client, upgrade, "store", false);
    harness
        .operation(
            client,
            upgrade,
            Operation::CreateIndex {
                store: name("store"),
                name: name("email"),
                key_path: KeyPath::String(name("email")),
                unique: true,
                multi_entry: false,
            },
        )
        .unwrap();
    let put = |key: f64, email: &str| Operation::Put {
        store: name("store"),
        value: value(b"x"),
        key: Some(number(key)),
        index_keys: vec![NamedIndexKeys {
            index: name("email"),
            keys: IndexKeys::Keys(vec![string(email)]),
        }],
        no_overwrite: false,
    };
    harness.operation(client, upgrade, put(1.0, "a@x.co")).unwrap();
    // Replacing the record that holds the index key is fine.
    harness.operation(client, upgrade, put(1.0, "a@x.co")).unwrap();
    let Err(error) = harness.operation(client, upgrade, put(2.0, "a@x.co")) else {
        panic!("the second record with the index key is refused");
    };
    assert_eq!(error.kind, ErrorKind::Constraint);
    // The failed request changed nothing.
    assert_eq!(harness.get(client, upgrade, "store", number(2.0)), None);

    let found = harness
        .operation(
            client,
            upgrade,
            Operation::Get {
                source: Source::Index {
                    store: name("store"),
                    index: name("email"),
                },
                range: KeyRange::only(string("a@x.co")),
            },
        )
        .unwrap();
    assert!(matches!(found, OperationResult::Value(Some(record)) if record.primary_key.value_as_double() == 1.0));
}

#[test]
fn cursors_walk_in_each_direction() {
    let mut harness = Harness::new();
    let client = harness.client();
    let (_, upgrade) = harness.open(client, "db", Some(1));
    let upgrade = upgrade.unwrap();
    harness.create_store(client, upgrade, "store", false);
    harness
        .operation(
            client,
            upgrade,
            Operation::CreateIndex {
                store: name("store"),
                name: name("letter"),
                key_path: KeyPath::String(name("letter")),
                unique: false,
                multi_entry: false,
            },
        )
        .unwrap();
    for (key, letter) in [(1.0, "a"), (2.0, "b"), (3.0, "a"), (4.0, "c"), (5.0, "b")] {
        harness
            .operation(
                client,
                upgrade,
                Operation::Put {
                    store: name("store"),
                    value: value(b"x"),
                    key: Some(number(key)),
                    index_keys: vec![NamedIndexKeys {
                        index: name("letter"),
                        keys: IndexKeys::Keys(vec![string(letter)]),
                    }],
                    no_overwrite: false,
                },
            )
            .unwrap();
    }

    let mut walk = |direction: Direction, cursor: CursorId| -> Vec<(String, f64)> {
        let source = Source::Index {
            store: name("store"),
            index: name("letter"),
        };
        let mut seen = Vec::new();
        let mut result = harness
            .operation(
                client,
                upgrade,
                Operation::OpenCursor {
                    cursor,
                    source,
                    range: KeyRange::unbounded(),
                    direction,
                    key_only: false,
                },
            )
            .unwrap();
        while let OperationResult::Cursor(Some(record)) = result {
            seen.push((
                String::from_utf16_lossy(&record.key.value_as_string().to_utf16()),
                record.primary_key.value_as_double(),
            ));
            result = harness
                .operation(
                    client,
                    upgrade,
                    Operation::IterateCursor {
                        cursor,
                        key: None,
                        primary_key: None,
                        count: 1,
                    },
                )
                .unwrap();
        }
        seen
    };

    let entries = |list: &[(&str, f64)]| {
        list.iter()
            .map(|(letter, key)| ((*letter).to_owned(), *key))
            .collect::<Vec<_>>()
    };
    assert_eq!(
        walk(Direction::Next, 1),
        entries(&[("a", 1.0), ("a", 3.0), ("b", 2.0), ("b", 5.0), ("c", 4.0)])
    );
    assert_eq!(
        walk(Direction::Prev, 2),
        entries(&[("c", 4.0), ("b", 5.0), ("b", 2.0), ("a", 3.0), ("a", 1.0)])
    );
    assert_eq!(
        walk(Direction::NextUnique, 3),
        entries(&[("a", 1.0), ("b", 2.0), ("c", 4.0)])
    );
    // prevunique reports the first record of each key.
    assert_eq!(
        walk(Direction::PrevUnique, 4),
        entries(&[("c", 4.0), ("b", 2.0), ("a", 1.0)])
    );
}

#[test]
fn a_cursor_continues_to_a_key_and_advances() {
    let mut harness = Harness::new();
    let client = harness.client();
    let connection = harness.database_with_records(
        client,
        &[(1.0, b"1"), (2.0, b"2"), (3.0, b"3"), (4.0, b"4"), (5.0, b"5")],
    );
    harness.transaction(client, connection, 1, &["store"], Mode::ReadOnly);
    let open = harness
        .operation(
            client,
            1,
            Operation::OpenCursor {
                cursor: 1,
                source: Source::ObjectStore(name("store")),
                range: KeyRange::unbounded(),
                direction: Direction::Next,
                key_only: true,
            },
        )
        .unwrap();
    assert!(
        matches!(open, OperationResult::Cursor(Some(record)) if record.key.value_as_double() == 1.0 && record.value.is_none())
    );
    let continued = harness
        .operation(
            client,
            1,
            Operation::IterateCursor {
                cursor: 1,
                key: Some(number(3.0)),
                primary_key: None,
                count: 1,
            },
        )
        .unwrap();
    assert!(matches!(continued, OperationResult::Cursor(Some(record)) if record.key.value_as_double() == 3.0));
    let advanced = harness
        .operation(
            client,
            1,
            Operation::IterateCursor {
                cursor: 1,
                key: None,
                primary_key: None,
                count: 2,
            },
        )
        .unwrap();
    assert!(matches!(advanced, OperationResult::Cursor(Some(record)) if record.key.value_as_double() == 5.0));
    let past_the_end = harness
        .operation(
            client,
            1,
            Operation::IterateCursor {
                cursor: 1,
                key: None,
                primary_key: None,
                count: 1,
            },
        )
        .unwrap();
    assert!(matches!(past_the_end, OperationResult::Cursor(None)));
}

#[test]
fn get_all_respects_range_count_and_direction() {
    let mut harness = Harness::new();
    let client = harness.client();
    let connection = harness.database_with_records(client, &[(1.0, b"1"), (2.0, b"2"), (3.0, b"3"), (4.0, b"4")]);
    harness.transaction(client, connection, 1, &["store"], Mode::ReadOnly);
    let mut get_all = |range: KeyRange, count: Option<u32>, direction: Direction| match harness
        .operation(
            client,
            1,
            Operation::GetAll {
                source: Source::ObjectStore(name("store")),
                range,
                count,
                direction,
                retrieve: Retrieve::Values,
            },
        )
        .unwrap()
    {
        OperationResult::Records(records) => numbers(&records),
        _ => panic!("getAll returns records"),
    };
    assert_eq!(
        get_all(KeyRange::unbounded(), None, Direction::Next),
        vec![1.0, 2.0, 3.0, 4.0]
    );
    assert_eq!(get_all(KeyRange::unbounded(), Some(2), Direction::Prev), vec![4.0, 3.0]);
    let range = KeyRange {
        lower: Some(number(2.0)),
        upper: Some(number(4.0)),
        lower_open: true,
        upper_open: false,
    };
    assert_eq!(get_all(range, None, Direction::Next), vec![3.0, 4.0]);
    let empty = KeyRange {
        lower: Some(number(3.0)),
        upper: Some(number(3.0)),
        lower_open: true,
        upper_open: false,
    };
    assert_eq!(get_all(empty, None, Direction::Next), Vec::<f64>::new());
}

#[test]
fn overlapping_read_write_transactions_run_one_after_the_other() {
    let mut harness = Harness::new();
    let client = harness.client();
    let connection = harness.database_with_records(client, &[]);
    harness.transaction(client, connection, 1, &["store"], Mode::ReadWrite);
    harness.transaction(client, connection, 2, &["store"], Mode::ReadWrite);

    // The second transaction's request waits until the first one finishes.
    let request = harness.request();
    harness.backend.handle(
        client,
        Request::Operation {
            transaction: 2,
            request,
            operation: Operation::Count {
                source: Source::ObjectStore(name("store")),
                range: KeyRange::unbounded(),
            },
        },
    );
    assert!(harness.events().is_empty());
    harness.put(client, 1, "store", Some(number(1.0)), b"one").unwrap();
    let events = harness.commit(client, 1);
    assert!(matches!(
        &events[..],
        [
            (_, Event::TransactionCompleted { transaction: 1 }),
            (
                _,
                Event::OperationSucceeded {
                    transaction: 2,
                    result: OperationResult::Count(1),
                    ..
                }
            ),
        ]
    ));
}

#[test]
fn a_departed_client_aborts_its_transactions_and_closes_its_connections() {
    let mut harness = Harness::new();
    let first = harness.client();
    let connection = harness.database_with_records(first, &[(1.0, b"one")]);
    harness.transaction(first, connection, 1, &["store"], Mode::ReadWrite);
    harness.put(first, 1, "store", Some(number(1.0)), b"changed").unwrap();
    harness.backend.disconnect_client(first);

    // A new version needs no other connection to close, so the upgrade starts at once.
    let second = harness.client();
    let (connection, upgrade) = harness.open(second, "db", Some(2));
    harness.commit(second, upgrade.unwrap());
    harness.transaction(second, connection, 1, &["store"], Mode::ReadOnly);
    assert_eq!(harness.get(second, 1, "store", number(1.0)), Some(b"one".to_vec()));
}

#[test]
fn requests_and_events_survive_the_wire() {
    use libipc_rust::wire::{Decoder, Encoder};

    let date = Key::create_date(1.5e12).as_ref().clone();
    let array = Key::create_array(vec![
        Key::create_number(1.0),
        Key::create_string(ak::Utf16String::from_utf8("x")),
    ])
    .as_ref()
    .clone();
    let request = Request::Operation {
        transaction: 7,
        request: 9,
        operation: Operation::Put {
            store: name("store"),
            value: value(b"payload"),
            key: Some(array.clone()),
            index_keys: vec![NamedIndexKeys {
                index: name("when"),
                keys: IndexKeys::Keys(vec![date]),
            }],
            no_overwrite: true,
        },
    };
    let encode = |request: &Request| {
        let mut encoder = Encoder::new();
        encoder.encode(request).unwrap();
        encoder.finish().0
    };
    let bytes = encode(&request);
    let decoded: Request = Decoder::new(&bytes, Vec::new()).decode().unwrap();
    assert_eq!(encode(&decoded), bytes);
    let Request::Operation {
        operation: Operation::Put { key: Some(key), .. },
        ..
    } = decoded
    else {
        panic!("the request decodes as it was");
    };
    assert_eq!(Key::compare_two_keys(&key, &array), 0);
}

#[test]
fn a_key_nested_too_deeply_is_refused() {
    use libipc_rust::wire::{Decoder, Encoder};

    let mut key = Key::create_number(1.0);
    for _ in 0..2000 {
        key = Key::create_array(vec![key]);
    }
    let mut encoder = Encoder::new();
    encoder.encode(key.as_ref()).unwrap();
    let (bytes, _) = encoder.finish();
    assert!(Decoder::new(&bytes, Vec::new()).decode::<Key>().is_err());
}

#[test]
fn an_index_population_skips_records_that_changed_meanwhile() {
    let mut harness = Harness::new();
    let client = harness.client();
    let (_, upgrade) = harness.open(client, "db", Some(1));
    let upgrade = upgrade.unwrap();
    harness.create_store(client, upgrade, "store", false);
    harness.put(client, upgrade, "store", Some(number(1.0)), b"a").unwrap();
    harness.put(client, upgrade, "store", Some(number(2.0)), b"b").unwrap();
    harness
        .operation(
            client,
            upgrade,
            Operation::CreateIndex {
                store: name("store"),
                name: name("idx"),
                key_path: KeyPath::String(name("k")),
                unique: true,
                multi_entry: false,
            },
        )
        .unwrap();
    let snapshot = harness
        .operation(
            client,
            upgrade,
            Operation::PopulateIndex {
                store: name("store"),
                index: name("idx"),
            },
        )
        .unwrap();
    assert!(matches!(&snapshot, OperationResult::Records(records) if records.len() == 2));

    // Record 2 changes before the agent's index keys arrive, and gets its index record from the put.
    harness
        .operation(
            client,
            upgrade,
            Operation::Put {
                store: name("store"),
                value: value(b"c"),
                key: Some(number(2.0)),
                index_keys: vec![NamedIndexKeys {
                    index: name("idx"),
                    keys: IndexKeys::Keys(vec![string("new")]),
                }],
                no_overwrite: false,
            },
        )
        .unwrap();
    // The stale key the agent computed for record 2 would collide with record 1's, and is skipped.
    harness
        .operation(
            client,
            upgrade,
            Operation::AddIndexKeys {
                store: name("store"),
                index: name("idx"),
                entries: vec![
                    IndexKeyEntry {
                        primary_key: number(1.0),
                        keys: vec![string("old")],
                    },
                    IndexKeyEntry {
                        primary_key: number(2.0),
                        keys: vec![string("old")],
                    },
                ],
            },
        )
        .unwrap();
    let count = |harness: &mut Harness, key: &str| match harness
        .operation(
            client,
            upgrade,
            Operation::Count {
                source: Source::Index {
                    store: name("store"),
                    index: name("idx"),
                },
                range: KeyRange::only(string(key)),
            },
        )
        .unwrap()
    {
        OperationResult::Count(count) => count,
        _ => panic!("count returns a count"),
    };
    assert_eq!(count(&mut harness, "old"), 1);
    assert_eq!(count(&mut harness, "new"), 1);

    // And each points to the right record: "old" to record 1, and "new" to record 2.
    let primary_key = |harness: &mut Harness, key: &str| match harness
        .operation(
            client,
            upgrade,
            Operation::GetKey {
                source: Source::Index {
                    store: name("store"),
                    index: name("idx"),
                },
                range: KeyRange::only(string(key)),
            },
        )
        .unwrap()
    {
        OperationResult::OptionalKey(Some(key)) => key.value_as_double(),
        _ => panic!("the index has the key"),
    };
    assert_eq!(primary_key(&mut harness, "old"), 1.0);
    assert_eq!(primary_key(&mut harness, "new"), 2.0);
}

#[test]
fn a_key_generator_past_2_to_the_53_fails() {
    let mut harness = Harness::new();
    let client = harness.client();
    let (_, upgrade) = harness.open(client, "db", Some(1));
    let upgrade = upgrade.unwrap();
    harness.create_store(client, upgrade, "store", true);
    harness
        .put(client, upgrade, "store", Some(number(9007199254740992.0)), b"a")
        .unwrap();
    let Err(error) = harness.put(client, upgrade, "store", None, b"b") else {
        panic!("the generator is exhausted");
    };
    assert_eq!(error.kind, ErrorKind::Constraint);
}

#[test]
fn a_queued_delete_waits_until_the_agent_handled_the_open_before_it() {
    let mut harness = Harness::new();
    let client = harness.client();
    let open = harness.request();
    harness.backend.handle(
        client,
        Request::Open {
            request: open,
            name: name("db"),
            version: Some(1),
        },
    );
    let delete = harness.request();
    harness.backend.handle(
        client,
        Request::DeleteDatabase {
            request: delete,
            name: name("db"),
        },
    );
    let events = harness.events();
    let Some((
        _,
        Event::UpgradeNeeded {
            connection,
            transaction,
            ..
        },
    )) = events.first().cloned()
    else {
        panic!("the open upgrades first");
    };
    harness.backend.handle(client, Request::Commit { transaction });
    let events = harness.events();
    assert!(
        events
            .iter()
            .any(|(_, event)| matches!(event, Event::OpenSucceeded { request, .. } if *request == open)),
        "the open succeeds once its upgrade commits"
    );
    assert!(
        events
            .iter()
            .all(|(_, event)| !matches!(event, Event::VersionChange { .. })),
        "the delete waits for the agent to handle the open"
    );
    harness.backend.handle(client, Request::OpenHandled { request: open });
    let events = harness.events();
    assert!(matches!(
        &events[..],
        [(_, Event::VersionChange { connection: target, new_version: None, .. })] if *target == connection
    ));
}

#[test]
fn a_generated_key_takes_the_placeholder_in_index_keys() {
    let mut harness = Harness::new();
    let client = harness.client();
    let (_, upgrade) = harness.open(client, "db", Some(1));
    let upgrade = upgrade.unwrap();
    harness
        .operation(
            client,
            upgrade,
            Operation::CreateObjectStore {
                name: name("store"),
                key_path: Some(KeyPath::String(name("id"))),
                auto_increment: true,
            },
        )
        .unwrap();
    harness
        .operation(
            client,
            upgrade,
            Operation::CreateIndex {
                store: name("store"),
                name: name("blob"),
                key_path: KeyPath::String(name("blob")),
                unique: false,
                multi_entry: false,
            },
        )
        .unwrap();
    harness
        .operation(
            client,
            upgrade,
            Operation::CreateIndex {
                store: name("store"),
                name: name("pair"),
                key_path: KeyPath::Sequence(vec![name("num"), name("id")]),
                unique: false,
                multi_entry: false,
            },
        )
        .unwrap();
    let placeholder = Key::create_binary(store::GENERATED_KEY_PLACEHOLDER.to_vec());
    let pair = Key::create_array(vec![Key::create_number(100.0), placeholder.clone()])
        .as_ref()
        .clone();
    harness
        .operation(
            client,
            upgrade,
            Operation::Put {
                store: name("store"),
                value: value(b"x"),
                key: None,
                // NB: The placeholder's bytes at another key path than the store's are the application's own binary key.
                index_keys: vec![
                    NamedIndexKeys {
                        index: name("pair"),
                        keys: IndexKeys::Keys(vec![pair]),
                    },
                    NamedIndexKeys {
                        index: name("blob"),
                        keys: IndexKeys::Keys(vec![placeholder.as_ref().clone()]),
                    },
                ],
                no_overwrite: false,
            },
        )
        .unwrap();
    let expected = Key::create_array(vec![Key::create_number(100.0), Key::create_number(1.0)])
        .as_ref()
        .clone();
    let found = harness
        .operation(
            client,
            upgrade,
            Operation::Get {
                source: Source::Index {
                    store: name("store"),
                    index: name("pair"),
                },
                range: KeyRange::only(expected),
            },
        )
        .unwrap();
    assert!(matches!(found, OperationResult::Value(Some(record)) if record.primary_key.value_as_double() == 1.0));
    let found = harness
        .operation(
            client,
            upgrade,
            Operation::Get {
                source: Source::Index {
                    store: name("store"),
                    index: name("blob"),
                },
                range: KeyRange::only(placeholder.as_ref().clone()),
            },
        )
        .unwrap();
    assert!(matches!(found, OperationResult::Value(Some(record)) if record.primary_key.value_as_double() == 1.0));
}

#[test]
fn a_primary_key_on_an_object_store_cursor_fails_the_request() {
    let mut harness = Harness::new();
    let client = harness.client();
    let connection = harness.database_with_records(client, &[(1.0, b"a"), (2.0, b"b")]);
    harness.transaction(client, connection, 1, &["store"], Mode::ReadOnly);
    harness
        .operation(
            client,
            1,
            Operation::OpenCursor {
                cursor: 1,
                source: Source::ObjectStore(name("store")),
                range: KeyRange::unbounded(),
                direction: Direction::Next,
                key_only: false,
            },
        )
        .unwrap();
    // A primary key is only for an index cursor, and only with a key; an agent that sends one otherwise gets an error.
    for key in [Some(number(2.0)), None] {
        let result = harness.operation(
            client,
            1,
            Operation::IterateCursor {
                cursor: 1,
                key,
                primary_key: Some(number(2.0)),
                count: 1,
            },
        );
        assert!(result.is_err());
    }
}

#[test]
fn a_disconnected_clients_open_waiting_on_its_own_connection_leaves_the_queue() {
    let mut harness = Harness::new();
    let first = harness.client();
    let (_, upgrade) = harness.open(first, "db", Some(1));
    harness.commit(first, upgrade.unwrap());

    // The first client asks for version 2 while its own version 1 connection is open, so the request waits for that
    // connection; then the client goes.
    let request = harness.request();
    harness.backend.handle(
        first,
        Request::Open {
            request,
            name: name("db"),
            version: Some(2),
        },
    );
    harness.events();
    harness.backend.disconnect_client(first);

    // Another client can still upgrade the database.
    let second = harness.client();
    let (_, upgrade) = harness.open(second, "db", Some(3));
    assert!(upgrade.is_some());
}

#[test]
fn a_long_chain_of_committing_transactions_finishes() {
    let mut harness = Harness::new();
    let client = harness.client();
    let connection = harness.database_with_records(client, &[]);

    // Every read/write transaction waits for the one before it, and asks to commit before it starts.
    const COUNT: u64 = 2_000;
    for id in 1..=COUNT {
        harness.transaction(client, connection, id, &["store"], Mode::ReadWrite);
        if id > 1 {
            harness.backend.handle(client, Request::Commit { transaction: id });
        }
    }
    harness.events();
    harness.backend.handle(client, Request::Commit { transaction: 1 });
    let completed = harness
        .events()
        .iter()
        .filter(|(_, event)| matches!(event, Event::TransactionCompleted { .. }))
        .count();
    assert_eq!(completed as u64, COUNT);
}

#[test]
fn a_put_past_the_storage_key_quota_fails() {
    let mut backend = Backend::new();
    // Each record below takes 48 bytes: an 8-byte number key and a 40-byte value.
    backend.set_limits(Limits {
        storage_key: 100,
        session: u64::MAX,
    });
    let mut harness = Harness::with_backend(backend);
    let client = harness.client();
    let connection = harness.database_with_records(client, &[(1.0, &[0; 40]), (2.0, &[0; 40])]);
    harness.transaction(client, connection, 1, &["store"], Mode::ReadWrite);
    let error = harness
        .put(client, 1, "store", Some(number(3.0)), &[0; 40])
        .err()
        .expect("the put fails");
    assert_eq!(error.kind, ErrorKind::QuotaExceeded);

    // Replacing a record gives back what it took, and so does deleting one.
    assert!(harness.put(client, 1, "store", Some(number(1.0)), &[1; 40]).is_ok());
    harness
        .operation(
            client,
            1,
            Operation::Delete {
                store: name("store"),
                range: KeyRange::only(number(2.0)),
            },
        )
        .unwrap();
    assert!(harness.put(client, 1, "store", Some(number(3.0)), &[0; 40]).is_ok());

    // An aborted put gives back what it took, too.
    harness.backend.handle(client, Request::Abort { transaction: 1 });
    harness.transaction(client, connection, 2, &["store"], Mode::ReadWrite);
    let error = harness
        .put(client, 2, "store", Some(number(3.0)), &[0; 40])
        .err()
        .expect("the put fails");
    assert_eq!(error.kind, ErrorKind::QuotaExceeded);
}

#[test]
fn the_session_limit_counts_every_storage_key() {
    let mut backend = Backend::new();
    backend.set_limits(Limits {
        storage_key: u64::MAX,
        session: 100,
    });
    let mut harness = Harness::with_backend(backend);
    let first = harness.client_of("https://a.example");
    harness.database_with_records(first, &[(1.0, &[0; 40])]);
    let second = harness.client_of("https://b.example");
    let connection = harness.database_with_records(second, &[(1.0, &[0; 40])]);
    harness.transaction(second, connection, 1, &["store"], Mode::ReadWrite);
    let error = harness
        .put(second, 1, "store", Some(number(2.0)), &[0; 40])
        .err()
        .expect("the put fails");
    assert_eq!(error.kind, ErrorKind::QuotaExceeded);
}

#[test]
fn a_new_index_past_the_quota_fails() {
    let mut backend = Backend::new();
    // Two records take 96 bytes; an index record for each takes 8 bytes of primary key and 40 of index key.
    backend.set_limits(Limits {
        storage_key: 150,
        session: u64::MAX,
    });
    let mut harness = Harness::with_backend(backend);
    let client = harness.client();
    let connection = harness.database_with_records(client, &[(1.0, &[0; 40]), (2.0, &[0; 40])]);
    harness.backend.handle(client, Request::Close { connection });
    let (_, upgrade) = harness.open(client, "db", Some(2));
    let upgrade = upgrade.unwrap();
    harness
        .operation(
            client,
            upgrade,
            Operation::CreateIndex {
                store: name("store"),
                name: name("idx"),
                key_path: KeyPath::String(name("k")),
                unique: false,
                multi_entry: false,
            },
        )
        .unwrap();
    harness
        .operation(
            client,
            upgrade,
            Operation::PopulateIndex {
                store: name("store"),
                index: name("idx"),
            },
        )
        .unwrap();
    let entries = [1.0, 2.0]
        .into_iter()
        .map(|key| IndexKeyEntry {
            primary_key: number(key),
            keys: vec![Key::create_binary(vec![0; 40]).as_ref().clone()],
        })
        .collect();
    let error = harness
        .operation(
            client,
            upgrade,
            Operation::AddIndexKeys {
                store: name("store"),
                index: name("idx"),
                entries,
            },
        )
        .err()
        .expect("the population fails");
    assert_eq!(error.kind, ErrorKind::QuotaExceeded);
}
