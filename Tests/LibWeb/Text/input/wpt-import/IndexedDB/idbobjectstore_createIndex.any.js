// META: title=IDBObjectStore.createIndex()
// META: global=window,worker
// META: script=resources/support.js

'use strict';

async_test(t => {
    let db;

    let open_rq = createdb(t);
    open_rq.onupgradeneeded = function (e) {
        db = e.target.result;
        let objStore = db.createObjectStore("store");
        let index = objStore.createIndex("index", "indexedProperty", { unique: true });

        assert_true(index instanceof IDBIndex, "IDBIndex");
        assert_equals(index.name, "index", "name");
        assert_equals(index.objectStore, objStore, "objectStore");
        assert_equals(index.keyPath, "indexedProperty", "keyPath");
        assert_true(index.unique, "unique");
        assert_false(index.multiEntry, "multiEntry");

        t.done();
    };
}, "Returns an IDBIndex and the properties are set correctly");

async_test(t => {
    let db, aborted,
        record = { indexedProperty: "bar" };

    let open_rq = createdb(t);
    open_rq.onupgradeneeded = function (e) {
        db = e.target.result;
        let txn = e.target.transaction,
            objStore = db.createObjectStore("store");

        objStore.add(record, 1);
        objStore.add(record, 2);
        let index = objStore.createIndex("index", "indexedProperty", { unique: true });

        assert_true(index instanceof IDBIndex, "IDBIndex");

        e.target.transaction.onabort = t.step_func(function (e) {
            aborted = true;
            assert_equals(e.type, "abort", "event type");
        });

        db.onabort = function (e) {
            assert_true(aborted, "transaction.abort event has fired");
            t.done();
        };

        e.target.transaction.oncomplete = fail(t, "got complete, expected abort");
    };
}, "Attempt to create an index that requires unique values on an object store already contains duplicates");

async_test(t => {
    let db, aborted;

    let open_rq = createdb(t);
    open_rq.onupgradeneeded = function (e) {
        db = e.target.result;
        let txn = e.target.transaction,
            objStore = db.createObjectStore("store", { keyPath: 'key' });

        for (let i = 0; i < 100; i++)
            objStore.add({ key: "key_" + i, indexedProperty: "indexed_" + i });

         let idx = objStore.createIndex("index", "indexedProperty")

        idx.get('indexed_99').onsuccess = t.step_func(function (e) {
            assert_equals(e.target.result.key, 'key_99', 'key');
        });
        idx.get('indexed_9').onsuccess = t.step_func(function (e) {
            assert_equals(e.target.result.key, 'key_9', 'key');
        });
    }

    open_rq.onsuccess = function () {
        t.done();
    }
}, "The index is usable right after being made");

async_test(t => {
    let db,
        events = [];

    let open_rq = createdb(t);
    open_rq.onupgradeneeded = function (e) {
        db = e.target.result;
        e.target.transaction.oncomplete = log("transaction.complete");

        let txn = e.target.transaction,
            objStore = db.createObjectStore("store");

        let rq_add1 = objStore.add({ animal: "Unicorn" }, 1);
        rq_add1.onsuccess = log("rq_add1.success");
        rq_add1.onerror = log("rq_add1.error");

        objStore.createIndex("index", "animal", { unique: true });

        let rq_add2 = objStore.add({ animal: "Unicorn" }, 2);
        rq_add2.onsuccess = log("rq_add2.success");
        rq_add2.onerror = function (e) {
            log("rq_add2.error")(e);
            e.preventDefault();
            e.stopPropagation();
        }

        objStore.deleteIndex("index");

        let rq_add3 = objStore.add({ animal: "Unicorn" }, 3);
        rq_add3.onsuccess = log("rq_add3.success");
        rq_add3.onerror = log("rq_add3.error");
    }

    open_rq.onsuccess = function (e) {
        log("open_rq.success")(e);
        assert_array_equals(events, ["rq_add1.success",
            "rq_add2.error: ConstraintError",
            "rq_add3.success",

            "transaction.complete",

            "open_rq.success"],
            "events");
        t.done();
    }

    function log(msg) {
        return function (e) {
            if (e && e.target && e.target.error)
                events.push(msg + ": " + e.target.error.name);
            else
                events.push(msg);
        };
    }
}, "Event ordering for a later deleted index");

async_test(t => {
    let db, aborted;

    let open_rq = createdb(t);
    open_rq.onupgradeneeded = function (e) {
        db = e.target.result;
        let txn = e.target.transaction,
            objStore = db.createObjectStore("store");

        for (let i = 0; i < 5; i++)
            objStore.add("object_" + i, i);

        let rq = objStore.createIndex("index", "")
        rq.onerror = function () { assert_unreached("error: " + rq.error.name); }
        rq.onsuccess = function () { }

        objStore.index("index")
            .get('object_4')
            .onsuccess = t.step_func(function (e) {
                assert_equals(e.target.result, 'object_4', 'result');
            });
    }

    open_rq.onsuccess = function () {
        t.done();
    }
}, "Empty keyPath");

async_test(t => {
    // Transaction may fire window.onerror in some implementations.
    setup({ allow_uncaught_exception: true });

    let db,
        events = [];

    let open_rq = createdb(t);
    open_rq.onupgradeneeded = function (e) {
        db = e.target.result;
        db.onerror = log("db.error");
        db.onabort = log("db.abort");
        e.target.transaction.onabort = log("transaction.abort")
        e.target.transaction.onerror = log("transaction.error")
        e.target.transaction.oncomplete = log("transaction.complete")

        let txn = e.target.transaction,
            objStore = db.createObjectStore("store");

        let rq_add1 = objStore.add({ animal: "Unicorn" }, 1);
        rq_add1.onsuccess = log("rq_add1.success");
        rq_add1.onerror = log("rq_add1.error");

        let rq_add2 = objStore.add({ animal: "Unicorn" }, 2);
        rq_add2.onsuccess = log("rq_add2.success");
        rq_add2.onerror = log("rq_add2.error");

        objStore.createIndex("index", "animal", { unique: true })

        let rq_add3 = objStore.add({ animal: "Unicorn" }, 3);
        rq_add3.onsuccess = log("rq_add3.success");
        rq_add3.onerror = log("rq_add3.error");
    }

    open_rq.onerror = function (e) {
        log("open_rq.error")(e);
        assert_array_equals(events, ["rq_add1.success",
            "rq_add2.success",

            "rq_add3.error: AbortError",
            "transaction.error: AbortError",
            "db.error: AbortError",

            "transaction.abort: ConstraintError",
            "db.abort: ConstraintError",

            "open_rq.error: AbortError"],
            "events");
        t.done();
    }

    function log(msg) {
        return function (e) {
            if (e && e.target && e.target.error)
                events.push(msg + ": " + e.target.error.name);
            else
                events.push(msg);
        };
    }
}, "Event order when unique constraint is triggered");

async_test(t => {
    setup({ allow_uncaught_exception: true });

    let db,
        events = [];

    const open_rq = createdb(t);
    open_rq.onupgradeneeded = function (e) {
        db = e.target.result;
        let txn = e.target.transaction;
        db.onerror = log("db.error");
        db.onabort = log("db.abort");
        txn.onabort = log("transaction.abort")
        txn.onerror = log("transaction.error")
        txn.oncomplete = log("transaction.complete")

        let objStore = db.createObjectStore("store");

        let rq_add1 = objStore.add({ animal: "Unicorn" }, 1);
        rq_add1.onsuccess = log("rq_add1.success");
        rq_add1.onerror = log("rq_add1.error");

        objStore.createIndex("index", "animal", { unique: true })

        let rq_add2 = objStore.add({ animal: "Unicorn" }, 2);
        rq_add2.onsuccess = log("rq_add2.success");
        rq_add2.onerror = log("rq_add2.error");

        let rq_add3 = objStore.add({ animal: "Horse" }, 3);
        rq_add3.onsuccess = log("rq_add3.success");
        rq_add3.onerror = log("rq_add3.error");
    }

    open_rq.onerror = function (e) {
        log("open_rq.error")(e);
        assert_array_equals(events, ["rq_add1.success",

            "rq_add2.error: ConstraintError",
            "transaction.error: ConstraintError",
            "db.error: ConstraintError",

            "rq_add3.error: AbortError",
            "transaction.error: AbortError",
            "db.error: AbortError",

            "transaction.abort: ConstraintError",
            "db.abort: ConstraintError",

            "open_rq.error: AbortError"],
            "events");
        t.done();
    }

    function log(msg) {
        return function (e) {
            if (e && e.target && e.target.error)
                events.push(msg + ": " + e.target.error.name);
            else
                events.push(msg);
        };
    }
}, "Event ordering for ConstraintError on request");

async_test(t => {
    let db,
        now = new Date(),
        mar18 = new Date(1111111111111),
        ar = ["Yay", 2, -Infinity],
        num = 1337;

    const open_rq = createdb(t);
    open_rq.onupgradeneeded = function (e) {
        db = e.target.result;
        let txn = e.target.transaction,
            objStore = db.createObjectStore("store", { keyPath: 'key' });

        objStore.add({ key: "now", i: now });
        objStore.add({ key: "mar18", i: mar18 });
        objStore.add({ key: "array", i: ar });
        objStore.add({ key: "number", i: num });

        let idx = objStore.createIndex("index", "i")

        idx.get(now).onsuccess = t.step_func(function (e) {
            assert_equals(e.target.result.key, 'now', 'key');
            assert_equals(e.target.result.i.getTime(), now.getTime(), 'getTime');
        });
        idx.get(mar18).onsuccess = t.step_func(function (e) {
            assert_equals(e.target.result.key, 'mar18', 'key');
            assert_equals(e.target.result.i.getTime(), mar18.getTime(), 'getTime');
        });
        idx.get(ar).onsuccess = t.step_func(function (e) {
            assert_equals(e.target.result.key, 'array', 'key');
            assert_array_equals(e.target.result.i, ar, 'array is the same');
        });
        idx.get(num).onsuccess = t.step_func(function (e) {
            assert_equals(e.target.result.key, 'number', 'key');
            assert_equals(e.target.result.i, num, 'number is the same');
        });
    }

    open_rq.onsuccess = function () {
        t.done();
    }
}, "Index can be valid keys");

async_test(t => {
    let db;

    const open_rq = createdb(t);
    open_rq.onupgradeneeded = function (e) {
        db = e.target.result
        let store = db.createObjectStore("store")

        for (let i = 0; i < 5; i++)
            store.add({ idx: "object_" + i }, i)

        store.createIndex("", "idx")

        store.index("")
            .get('object_4')
            .onsuccess = t.step_func(function (e) {
                assert_equals(e.target.result.idx, 'object_4', 'result')
            })
        assert_equals(store.indexNames[0], "", "indexNames[0]")
        assert_equals(store.indexNames.length, 1, "indexNames.length")
    }

    open_rq.onsuccess = function () {
        let store = db.transaction("store", "readonly").objectStore("store")

        assert_equals(store.indexNames[0], "", "indexNames[0]")
        assert_equals(store.indexNames.length, 1, "indexNames.length")

        t.done()
    }
}, "IDBObjectStore.createIndex() - empty name");

async_test(t => {
    const open_rq = createdb(t);

    open_rq.onupgradeneeded = function (e) {
        let db = e.target.result;
        let ostore = db.createObjectStore("store");
        ostore.createIndex("a", "a");
        assert_throws_dom("ConstraintError", function () {
            ostore.createIndex("a", "a");
        });
        t.done();
    }
}, "If an index with the name name already exists in this object store, the implementation must throw a DOMException of type ConstraintError");

async_test(t => {
    const open_rq = createdb(t);

    open_rq.onupgradeneeded = function (e) {
        let db = e.target.result;
        let ostore = db.createObjectStore("store");
        assert_throws_dom("SyntaxError", function () {
            ostore.createIndex("ab", ".");
        });
        t.done();
    }
}, "If keyPath is not a valid key path, the implementation must throw a DOMException of type SyntaxError");

async_test(t => {
    let db, ostore;

    let open_rq = createdb(t);
    open_rq.onupgradeneeded = function (event) {
        db = event.target.result;
        ostore = db.createObjectStore("store");
        db.deleteObjectStore("store");
    }

    open_rq.onsuccess = function (event) {
        t.step(function () {
            assert_throws_dom("InvalidStateError", function () {
                ostore.createIndex("index", "indexedProperty");
            });
        });
        t.done();
    }
}, "If the object store has been deleted, the implementation must throw a DOMException of type InvalidStateError");

async_test(t => {
    let db;

    const open_rq = createdb(t);
    open_rq.onupgradeneeded = function (event) {
        db = event.target.result;
        db.createObjectStore("store");
    }

    open_rq.onsuccess = function (event) {
        let txn = db.transaction("store", "readwrite");
        let ostore = txn.objectStore("store");
        t.step(function () {
            assert_throws_dom("InvalidStateError", function () {
                ostore.createIndex("index", "indexedProperty");
            });
        });
        t.done();
    }
}, "Operate out versionchange throw InvalidStateError");

/* IndexedDB: Exception Order of IDBObjectStore.createIndex() */
indexeddb_test(
    function (t, db, txn) {
        let store = db.createObjectStore("s");
    },
    function (t, db) {
        let txn = db.transaction("s", "readonly");
        let store = txn.objectStore("s");
        txn.oncomplete = function () {
            assert_throws_dom("InvalidStateError", function () {
                store.createIndex("index", "foo");
            });
            t.done();
        };
    },
    "InvalidStateError(Incorrect mode) vs. TransactionInactiveError. Mode check should precede state check of the transaction."
);

let gDeletedObjectStore;
indexeddb_test(
    function (t, db, txn) {
        gDeletedObjectStore = db.createObjectStore("s");
        db.deleteObjectStore("s");
        txn.oncomplete = function () {
            assert_throws_dom("InvalidStateError", function () {
                gDeletedObjectStore.createIndex("index", "foo");
            });
            t.done();
        };
    },
    null,
    "InvalidStateError(Deleted ObjectStore) vs. TransactionInactiveError. Deletion check should precede transaction-state check."
);

indexeddb_test(
    function (t, db, txn) {
        let store = db.createObjectStore("s");
        store.createIndex("index", "foo");
        txn.oncomplete = function () {
            assert_throws_dom("TransactionInactiveError", function () {
                store.createIndex("index", "foo");
            });
            t.done();
        };
    },
    null,
    "TransactionInactiveError vs. ConstraintError. Transaction-state check should precede index name check."
);

indexeddb_test(
    function (t, db) {
        let store = db.createObjectStore("s");
        store.createIndex("index", "foo");
        assert_throws_dom("ConstraintError", function () {
            store.createIndex("index", "invalid key path");
        });
        assert_throws_dom("ConstraintError", function () {
            store.createIndex("index",
                ["invalid key path 1", "invalid key path 2"]);
        });
        t.done();
    },
    null,
    "ConstraintError vs. SyntaxError. Index name check should precede syntax check of the key path"
);

indexeddb_test(
    function (t, db) {
        let store = db.createObjectStore("s");
        assert_throws_dom("SyntaxError", function () {
            store.createIndex("index",
                ["invalid key path 1", "invalid key path 2"],
                { multiEntry: true });
        });
        t.done();
    },
    null,
    "SyntaxError vs. InvalidAccessError. Syntax check should precede multiEntry check of the key path."
);

/* AutoIncrement in Compound Index */
indexeddb_test(
    function (t, db, txn) {
        // No auto-increment
        let store = db.createObjectStore("Store1", { keyPath: "id" });
        store.createIndex("CompoundKey", ["num", "id"]);

        // Add data
        store.put({ id: 1, num: 100 });
    },
    function (t, db) {
        let store = db.transaction("Store1", "readwrite").objectStore("Store1");

        store.openCursor().onsuccess = t.step_func(function (e) {
            let item = e.target.result.value;
            store.index("CompoundKey").get([item.num, item.id]).onsuccess = t.step_func(function (e) {
                assert_equals(e.target.result ? e.target.result.num : null, 100, 'Expected 100.');
                t.done();
            });
        });
    },
    "Explicit Primary Key"
);

indexeddb_test(
    function (t, db, txn) {
        // Auto-increment
        let store = db.createObjectStore("Store2", { keyPath: "id", autoIncrement: true });
        store.createIndex("CompoundKey", ["num", "id"]);

        // Add data
        store.put({ num: 100 });
    },
    function (t, db) {
        let store = db.transaction("Store2", "readwrite").objectStore("Store2");
        store.openCursor().onsuccess = t.step_func(function (e) {
            let item = e.target.result.value;
            store.index("CompoundKey").get([item.num, item.id]).onsuccess = t.step_func(function (e) {
                assert_equals(e.target.result ? e.target.result.num : null, 100, 'Expected 100.');
                t.done();
            });
        });
    },
    "Auto-Increment Primary Key"
);

indexeddb_test(
    function (t, db, txn) {
        // Auto-increment
        let store = db.createObjectStore("Store3", { keyPath: "id", autoIncrement: true });
        store.createIndex("CompoundKey", ["num", "id", "other"]);

        let num = 100;

        // Add data to Store3 - valid keys
        // Objects will be stored in Store3 and keys will get added
        // to the CompoundKeys index.
        store.put({ num: num++, other: 0 });
        store.put({ num: num++, other: [0] });

        // Add data - missing key
        // Objects will be stored in Store3 but keys won't get added to
        // the CompoundKeys index because the 'other' keypath doesn't
        // resolve to a value.
        store.put({ num: num++ });

        // Add data to Store3 - invalid keys
        // Objects will be stored in Store3 but keys won't get added to
        // the CompoundKeys index because the 'other' property values
        // aren't valid keys.
        store.put({ num: num++, other: null });
        store.put({ num: num++, other: {} });
        store.put({ num: num++, other: [null] });
        store.put({ num: num++, other: [{}] });
    },
    function (t, db) {
        let store = db.transaction("Store3", "readwrite").objectStore("Store3");
        const keys = [];
        let count;
        store.count().onsuccess = t.step_func(e => { count = e.target.result; });
        store.index("CompoundKey").openCursor().onsuccess = t.step_func(function (e) {
            const cursor = e.target.result;
            if (cursor !== null) {
                keys.push(cursor.key);
                cursor.continue();
                return;
            }

            // Done iteration, check results.
            assert_equals(count, 7, 'Expected all 7 records to be stored.');
            assert_equals(keys.length, 2, 'Expected exactly two index entries.');
            assert_array_equals(keys[0], [100, 1, 0]);
            assert_object_equals(keys[1], [101, 2, [0]]);
            t.done();
        });
    },
    "Auto-Increment Primary Key - invalid key values elsewhere"
);