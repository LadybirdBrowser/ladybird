// META: title=IDBFactory.open()
// META: global=window,worker
// META: script=resources/support.js
// @author Microsoft <https://www.microsoft.com>
// @author Odin H�rthe Omdal <mailto:odinho@opera.com>

'use strict';

async_test(t => {
    const open_rq = createdb(t, undefined, 9);

    open_rq.onupgradeneeded = function (e) { };
    open_rq.onsuccess = function (e) {
        assert_equals(e.target.source, null, "source")
        t.done();
    }
}, "IDBFactory.open() - request has no source");

async_test(t => {
    let database_name = location + '-database_name';
    const open_rq = createdb(t, database_name, 13);

    open_rq.onupgradeneeded = function (e) { };
    open_rq.onsuccess = function (e) {
        let db = e.target.result;
        assert_equals(db.name, database_name, 'db.name');
        assert_equals(db.version, 13, 'db.version');
        t.done();
    }
}, "IDBFactory.open() - database 'name' and 'version' are correctly set");

async_test(t => {
    const open_rq = createdb(t, undefined, 13);
    let did_upgrade = false;

    open_rq.onupgradeneeded = function () { };
    open_rq.onsuccess = function (e) {
        let db = e.target.result;
        db.close();

        let open_rq2 = indexedDB.open(db.name);
        open_rq2.onsuccess = t.step_func(function (e) {
            assert_equals(e.target.result.version, 13, "db.version")
            e.target.result.close();
            t.done();
        });
        open_rq2.onupgradeneeded = fail(t, 'Unexpected upgradeneeded')
        open_rq2.onerror = fail(t, 'Unexpected error')
    }
}, "IDBFactory.open() - no version opens current database");

async_test(t => {
    const open_rq = createdb(t, self.location + '-database_name_new');
    open_rq.onupgradeneeded = function (e) {
        assert_equals(e.target.result.version, 1, "db.version");
    };
    open_rq.onsuccess = function (e) {
        assert_equals(e.target.result.version, 1, "db.version");
        t.done();
    };
}, "IDBFactory.open() - new database has default version");

async_test(t => {
    const open_rq = createdb(t, self.location + '-database_name');

    open_rq.onupgradeneeded = function () { };
    open_rq.onsuccess = function (e) {
        assert_equals(e.target.result.objectStoreNames.length, 0, "objectStoreNames.length");
        t.done();
    };
}, "IDBFactory.open() - new database is empty");

async_test(t => {
    const open_rq = createdb(t, undefined, 13);
    let did_upgrade = false;
    let open_rq2;

    open_rq.onupgradeneeded = function () { };
    open_rq.onsuccess = function (e) {
        let db = e.target.result;
        db.close();

        open_rq2 = indexedDB.open(db.name, 14);
        open_rq2.onupgradeneeded = function () { };
        open_rq2.onsuccess = t.step_func(open_previous_db);
        open_rq2.onerror = fail(t, 'Unexpected error')
    }

    function open_previous_db(e) {
        let open_rq3 = indexedDB.open(e.target.result.name, 13);
        open_rq3.onerror = t.step_func(function (e) {
            assert_equals(e.target.error.name, 'VersionError', 'e.target.error.name')
            open_rq2.result.close();
            t.done();
        });
        open_rq3.onupgradeneeded = fail(t, 'Unexpected upgradeneeded')
        open_rq3.onsuccess = fail(t, 'Unexpected success')
    }
}, "IDBFactory.open() - open database with a lower version than current");

async_test(t => {
    const open_rq = createdb(t, undefined, 13);
    let did_upgrade = false;
    let open_rq2;

    open_rq.onupgradeneeded = function () { };
    open_rq.onsuccess = function (e) {
        let db = e.target.result;
        db.close();

        open_rq2 = indexedDB.open(db.name, 14);
        open_rq2.onupgradeneeded = function () {
            did_upgrade = true;
        };
        open_rq2.onsuccess = t.step_func(open_current_db);
        open_rq2.onerror = fail(t, 'Unexpected error')
    }

    function open_current_db(e) {
        let open_rq3 = indexedDB.open(e.target.result.name);
        open_rq3.onsuccess = t.step_func(function (e) {
            assert_equals(e.target.result.version, 14, "db.version")
            open_rq2.result.close();
            open_rq3.result.close();
            t.done();
        });
        open_rq3.onupgradeneeded = fail(t, 'Unexpected upgradeneeded')
        open_rq3.onerror = fail(t, 'Unexpected error')

        assert_true(did_upgrade, 'did upgrade');
    }
}, "IDBFactory.open() - open database with a higher version than current");

async_test(t => {
    const open_rq = createdb(t, undefined, 13);
    let did_upgrade = false;
    let did_db_abort = false;

    open_rq.onupgradeneeded = function (e) {
        did_upgrade = true;
        e.target.result.onabort = function () {
            did_db_abort = true;
        }
        e.target.transaction.abort();
    };
    open_rq.onerror = function (e) {
        assert_true(did_upgrade);
        assert_equals(e.target.error.name, 'AbortError', 'target.error');
        t.done()
    };
}, "IDBFactory.open() - error in version change transaction aborts open");

function should_throw(val, name) {
    if (!name) {
        name = ((typeof val == "object" && val) ? "object" : format_value(val))
    }
    test(function () {
        assert_throws_js(TypeError, function () {
            indexedDB.open('test', val);
        });
    }, "Calling open() with version argument " + name + " should throw TypeError.")
}

should_throw(-1)
should_throw(-0.5)
should_throw(0)
should_throw(0.5)
should_throw(0.8)
should_throw(0x20000000000000)  // Number.MAX_SAFE_INTEGER + 1
should_throw(NaN)
should_throw(Infinity)
should_throw(-Infinity)
should_throw("foo")
should_throw(null)
should_throw(false)

should_throw({
    toString: function () { assert_unreached("toString should not be called for ToPrimitive [Number]"); },
    valueOf: function () { return 0; }
})
should_throw({
    toString: function () { return 0; },
    valueOf: function () { return {}; }
}, 'object (second)')
should_throw({
    toString: function () { return {}; },
    valueOf: function () { return {}; },
}, 'object (third)')


/* Valid */

function should_work(val, expected_version) {
    let name = format_value(val);
    let dbname = 'test-db-does-not-exist';
    async_test(function (t) {
        indexedDB.deleteDatabase(dbname);
        let rq = indexedDB.open(dbname, val);
        rq.onupgradeneeded = t.step_func(function () {
            let db = rq.result;
            assert_equals(db.version, expected_version, 'version');
            rq.transaction.abort();
        });
        rq.onsuccess = t.unreached_func("open should fail");
        rq.onerror = t.step_func(function () {
            t.done()
        });
    }, "Calling open() with version argument " + name + " should not throw.");
}

should_work(1.5, 1)
should_work(Number.MAX_SAFE_INTEGER, Number.MAX_SAFE_INTEGER)  // 0x20000000000000 - 1
should_work(undefined, 1);

async_test(t => {
    let db, db2;
    const open_rq = createdb(t, undefined, 9);

    open_rq.onupgradeneeded = function (e) {
        db = e.target.result;

        let st = db.createObjectStore("store");
        st.createIndex("index", "i");

        assert_equals(db.version, 9, "first db.version");
        assert_true(db.objectStoreNames.contains("store"), "objectStoreNames contains store");
        assert_true(st.indexNames.contains("index"), "indexNames contains index");

        st.add({ i: "Joshua" }, 1);
        st.add({ i: "Jonas" }, 2);
    };
    open_rq.onsuccess = function (e) {
        db.close();
        let open_rq2 = indexedDB.open(db.name, 10);
        open_rq2.onupgradeneeded = t.step_func(function (e) {
            db2 = e.target.result;

            db2.createObjectStore("store2");

            let store = open_rq2.transaction.objectStore("store")
            store.createIndex("index2", "i");

            assert_equals(db2.version, 10, "db2.version");

            assert_true(db2.objectStoreNames.contains("store"), "second objectStoreNames contains store");
            assert_true(db2.objectStoreNames.contains("store2"), "second objectStoreNames contains store2");
            assert_true(store.indexNames.contains("index"), "second indexNames contains index");
            assert_true(store.indexNames.contains("index2"), "second indexNames contains index2");

            store.add({ i: "Odin" }, 3);
            store.put({ i: "Sicking" }, 2);

            open_rq2.transaction.abort();
        });
        open_rq2.onerror = t.step_func(function (e) {
            assert_equals(db2.version, 9, "db2.version after error");
            assert_true(db2.objectStoreNames.contains("store"), "objectStoreNames contains store after error");
            assert_false(db2.objectStoreNames.contains("store2"), "objectStoreNames not contains store2 after error");

            let open_rq3 = indexedDB.open(db.name);
            open_rq3.onsuccess = t
                .step_func(function (e) {
                    let db3 = e.target.result;

                    assert_true(db3.objectStoreNames.contains("store"), "third objectStoreNames contains store");
                    assert_false(db3.objectStoreNames.contains("store2"), "third objectStoreNames contains store2");

                    let st = db3.transaction("store", "readonly").objectStore("store");

                    assert_equals(db3.version, 9, "db3.version");

                    assert_true(st.indexNames.contains("index"), "third indexNames contains index");
                    assert_false(st.indexNames.contains("index2"), "third indexNames contains index2");

                    st.openCursor(null, "prev").onsuccess = t.step_func(function (e) {
                        assert_equals(e.target.result.key, 2, "opencursor(prev) key");
                        assert_equals(e.target.result.value.i, "Jonas", "opencursor(prev) value");
                    });
                    st.get(3).onsuccess = t.step_func(function (e) {
                        assert_equals(e.target.result, undefined, "get(3)");
                    });

                    let idx = st.index("index");
                    idx.getKey("Jonas").onsuccess = t.step_func(function (e) {
                        assert_equals(e.target.result, 2, "getKey(Jonas)");
                    });
                    idx.getKey("Odin").onsuccess = t.step_func(function (e) {
                        assert_equals(e.target.result, undefined, "getKey(Odin)");
                    });
                    idx.getKey("Sicking").onsuccess = t.step_func(function (e) {
                        assert_equals(e.target.result, undefined, "getKey(Sicking)");

                        db3.close();
                        t.done();
                    });
                });
        });
    };
}, "IDBFactory.open() - error in upgradeneeded resets db");

async_test(t => {
    let db;
    let count_done = 0;
    const open_rq = createdb(t);

    open_rq.onupgradeneeded = function (e) {
        db = e.target.result;

        db.createObjectStore("store");
        assert_true(db.objectStoreNames.contains("store"), "objectStoreNames contains store");

        let store = e.target.transaction.objectStore("store");
        assert_equals(store.name, "store", "store.name");

        store.add("data", 1);

        store.count().onsuccess = t.step_func(function (e) {
            assert_equals(e.target.result, 1, "count()");
            count_done++;
        });

        store.add("data2", 2);
    };
    open_rq.onsuccess = function (e) {
        let store = db.transaction("store", "readonly").objectStore("store");
        assert_equals(store.name, "store", "store.name");
        store.count().onsuccess = t.step_func(function (e) {
            assert_equals(e.target.result, 2, "count()");
            count_done++;
        });
        db.close();

        let open_rq2 = indexedDB.open(db.name, 10);
        open_rq2.onupgradeneeded = t.step_func(function (e) {
            let db2 = e.target.result;
            assert_true(db2.objectStoreNames.contains("store"), "objectStoreNames contains store");
            let store = open_rq2.transaction.objectStore("store");
            assert_equals(store.name, "store", "store.name");

            store.add("data3", 3);

            store.count().onsuccess = t.step_func(function (e) {
                assert_equals(e.target.result, 3, "count()");
                count_done++;

                assert_equals(count_done, 3, "count_done");

                db2.close();
                t.done();
            });
        });
    };
}, "IDBFactory.open() - second open's transaction is available to get objectStores");

async_test(t => {
    let db;
    let open_rq = createdb(t, undefined, 9);
    let open2_t = t;

    open_rq.onupgradeneeded = function (e) {
        db = e.target.result;

        assert_true(e instanceof IDBVersionChangeEvent, "e instanceof IDBVersionChangeEvent");
        assert_equals(e.oldVersion, 0, "oldVersion");
        assert_equals(e.newVersion, 9, "newVersion");
        assert_equals(e.type, "upgradeneeded", "event type");

        assert_equals(db.version, 9, "db.version");
    };
    open_rq.onsuccess = function (e) {
        assert_true(e instanceof Event, "e instanceof Event");
        assert_false(e instanceof IDBVersionChangeEvent, "e not instanceof IDBVersionChangeEvent");
        assert_equals(e.type, "success", "event type");
        t.done();


        /**
         * Second test
         */
        db.onversionchange = function () { db.close(); };

        let open_rq2 = createdb(open2_t, db.name, 10);
        open_rq2.onupgradeneeded = function (e) {
            let db2 = e.target.result;
            assert_true(e instanceof IDBVersionChangeEvent, "e instanceof IDBVersionChangeEvent");
            assert_equals(e.oldVersion, 9, "oldVersion");
            assert_equals(e.newVersion, 10, "newVersion");
            assert_equals(e.type, "upgradeneeded", "event type");

            assert_equals(db2.version, 10, "new db.version");

            t.done();
        };
    };
}, "IDBFactory.open() - upgradeneeded gets VersionChangeEvent");