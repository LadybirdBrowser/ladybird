describe("IC cache with non-extensible objects", () => {
    test("AddOwnProperty cache should not apply to frozen objects", () => {
        function setX(o, v) {
            o.x = v;
        }

        const normal = {};
        const frozen = Object.freeze({});

        setX(normal, 1);
        setX(frozen, 2);

        expect(normal.x).toBe(1);
        expect(frozen.x).toBeUndefined();
        expect(Object.isFrozen(frozen)).toBeTrue();
    });

    test("AddOwnProperty cache should not apply to sealed objects", () => {
        function setX(o, v) {
            o.x = v;
        }

        const normal = {};
        const sealed = Object.seal({});

        setX(normal, 1);
        setX(sealed, 2);

        expect(normal.x).toBe(1);
        expect(sealed.x).toBeUndefined();
        expect(Object.isSealed(sealed)).toBeTrue();
    });

    test("AddOwnProperty cache should not apply to non-extensible objects", () => {
        function setX(o, v) {
            o.x = v;
        }

        const normal = {};
        const nonExtensible = Object.preventExtensions({});

        setX(normal, 1);
        setX(nonExtensible, 2);

        expect(normal.x).toBe(1);
        expect(nonExtensible.x).toBeUndefined();
        expect(Object.isExtensible(nonExtensible)).toBeFalse();
    });

    test("Polymorphic AddOwnProperty with mixed extensibility", () => {
        function setX(o, v) {
            o.x = v;
        }

        const objects = [{}, Object.freeze({}), {}, Object.seal({}), {}, Object.preventExtensions({})];

        for (let i = 0; i < objects.length; i++) {
            setX(objects[i], i);
        }

        expect(objects[0].x).toBe(0);
        expect(objects[1].x).toBeUndefined();
        expect(objects[2].x).toBe(2);
        expect(objects[3].x).toBeUndefined();
        expect(objects[4].x).toBe(4);
        expect(objects[5].x).toBeUndefined();
    });

    test("Add property in loop with some non-extensible objects", () => {
        function setVal(o, v) {
            o.value = v;
        }

        const results = [];
        for (let i = 0; i < 10; i++) {
            const obj = i % 3 === 0 ? Object.freeze({}) : {};
            setVal(obj, i);
            results.push(obj.value);
        }

        expect(results[0]).toBeUndefined();
        expect(results[1]).toBe(1);
        expect(results[2]).toBe(2);
        expect(results[3]).toBeUndefined();
        expect(results[4]).toBe(4);
        expect(results[5]).toBe(5);
        expect(results[6]).toBeUndefined();
        expect(results[7]).toBe(7);
        expect(results[8]).toBe(8);
        expect(results[9]).toBeUndefined();
    });

    test("Sealed object allows modification but not addition", () => {
        function setX(o, v) {
            o.x = v;
        }

        const sealed = Object.seal({ x: 100 });
        const normal = {};

        setX(normal, 1);
        setX(sealed, 200);

        expect(normal.x).toBe(1);
        expect(sealed.x).toBe(200);
    });

    test("Frozen object does not allow modification", () => {
        function setX(o, v) {
            o.x = v;
        }

        const frozen = Object.freeze({ x: 100 });
        const normal = {};

        setX(normal, 1);
        setX(frozen, 200);

        expect(normal.x).toBe(1);
        expect(frozen.x).toBe(100);
    });
});
