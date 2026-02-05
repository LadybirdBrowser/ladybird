/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

describe("dictionary mode objects remain cacheable after property deletion", () => {
    const DICTIONARY_THRESHOLD = 100;

    function makeDictionaryObject(count = DICTIONARY_THRESHOLD) {
        let obj = {};
        for (let i = 0; i < count; i++) {
            obj["prop" + i] = i;
        }
        return obj;
    }

    test("basic delete preserves other properties", () => {
        let obj = makeDictionaryObject();
        let mid = Math.floor(DICTIONARY_THRESHOLD / 2);
        delete obj["prop" + mid];

        for (let i = 0; i < DICTIONARY_THRESHOLD; i++) {
            if (i === mid) {
                expect(obj["prop" + i]).toBeUndefined();
                expect("prop" + mid in obj).toBeFalse();
            } else {
                expect(obj["prop" + i]).toBe(i);
            }
        }
    });

    test("multiple deletes preserve remaining properties", () => {
        let obj = makeDictionaryObject();
        let deleted = [0, 10, 50, 90, 99];
        for (let idx of deleted) {
            delete obj["prop" + idx];
        }

        for (let i = 0; i < DICTIONARY_THRESHOLD; i++) {
            if (deleted.includes(i)) {
                expect(obj["prop" + i]).toBeUndefined();
            } else {
                expect(obj["prop" + i]).toBe(i);
            }
        }
    });

    test("delete then add new property", () => {
        let obj = makeDictionaryObject();
        let mid = Math.floor(DICTIONARY_THRESHOLD / 2);
        delete obj["prop" + mid];

        obj.newProp = "new value";

        expect(obj["prop" + mid]).toBeUndefined();
        expect(obj.newProp).toBe("new value");
        expect(obj["prop" + (mid - 1)]).toBe(mid - 1);
        expect(obj["prop" + (mid + 1)]).toBe(mid + 1);
    });

    test("repeated access after delete (cache invalidation)", () => {
        let obj = makeDictionaryObject();

        // Populate caches
        for (let j = 0; j < 10; j++) {
            for (let i = 0; i < DICTIONARY_THRESHOLD; i++) {
                void obj["prop" + i];
            }
        }

        let mid = Math.floor(DICTIONARY_THRESHOLD / 2);
        delete obj["prop" + mid];

        // Access again after delete
        for (let i = 0; i < DICTIONARY_THRESHOLD; i++) {
            if (i === mid) {
                expect(obj["prop" + i]).toBeUndefined();
            } else {
                expect(obj["prop" + i]).toBe(i);
            }
        }
    });

    test("delete with accessor properties", () => {
        let obj = {};
        let accessorCalls = 0;

        for (let i = 0; i < 50; i++) {
            obj["prop" + i] = i;
        }

        Object.defineProperty(obj, "accessor", {
            get() {
                accessorCalls++;
                return 999;
            },
            configurable: true,
        });

        for (let i = 50; i < 100; i++) {
            obj["prop" + i] = i;
        }

        delete obj.prop25;
        delete obj.prop75;

        expect(obj.accessor).toBe(999);
        expect(accessorCalls).toBe(1);
        expect(obj.prop24).toBe(24);
        expect(obj.prop26).toBe(26);
        expect(obj.prop74).toBe(74);
        expect(obj.prop76).toBe(76);
    });

    test("delete and Object.keys/values/entries", () => {
        let obj = {};
        for (let i = 0; i < 10; i++) {
            obj["prop" + i] = i;
        }

        delete obj.prop5;

        expect(Object.keys(obj)).toHaveLength(9);
        expect(Object.values(obj)).toHaveLength(9);
        expect(Object.entries(obj)).toHaveLength(9);
        expect(Object.keys(obj).includes("prop5")).toBeFalse();
    });

    test("stress test - many deletes", () => {
        let obj = {};
        for (let i = 0; i < 1000; i++) {
            obj["prop" + i] = i;
        }

        for (let i = 0; i < 1000; i += 2) {
            delete obj["prop" + i];
        }

        for (let i = 0; i < 1000; i++) {
            if (i % 2 === 0) {
                expect(obj["prop" + i]).toBeUndefined();
            } else {
                expect(obj["prop" + i]).toBe(i);
            }
        }
    });

    test("delete and hasOwnProperty", () => {
        let obj = makeDictionaryObject(50);
        delete obj.prop25;

        expect(obj.hasOwnProperty("prop25")).toBeFalse();
        expect(obj.hasOwnProperty("prop24")).toBeTrue();
        expect(obj.hasOwnProperty("prop26")).toBeTrue();
    });

    test("delete and property descriptor", () => {
        let obj = makeDictionaryObject(50);
        delete obj.prop25;

        expect(Object.getOwnPropertyDescriptor(obj, "prop25")).toBeUndefined();
        expect(Object.getOwnPropertyDescriptor(obj, "prop24").value).toBe(24);
        expect(Object.getOwnPropertyDescriptor(obj, "prop26").value).toBe(26);
    });
});
