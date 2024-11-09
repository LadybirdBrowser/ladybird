test("length is 1", () => {
    expect(Array.fromAsync).toHaveLength(1);
});

describe("normal behavior", () => {
    function checkResult(promise, type = Array) {
        expect(promise).toBeInstanceOf(Promise);
        let error = null;
        let passed = false;
        promise
            .then(value => {
                expect(value instanceof type).toBeTrue();
                expect(value[0]).toBe(0);
                expect(value[1]).toBe(2);
                expect(value[2]).toBe(4);
                passed = true;
            })
            .catch(value => {
                error = value;
            });
        runQueuedPromiseJobs();
        expect(error).toBeNull();
        expect(passed).toBeTrue();
    }

    test("async from sync iterable no mapfn", () => {
        const input = [0, Promise.resolve(2), Promise.resolve(4)].values();
        const promise = Array.fromAsync(input);
        checkResult(promise);
    });

    test("from object of promises no mapfn", () => {
        let promise = Array.fromAsync({
            length: 3,
            0: Promise.resolve(0),
            1: Promise.resolve(2),
            2: Promise.resolve(4),
        });
        checkResult(promise);
    });

    test("async from sync iterable with mapfn", () => {
        const input = [Promise.resolve(0), 1, Promise.resolve(2)].values();
        const promise = Array.fromAsync(input, async element => element * 2);
        checkResult(promise);
    });

    test("from object of promises with mapfn", () => {
        let promise = Array.fromAsync(
            {
                length: 3,
                0: Promise.resolve(0),
                1: Promise.resolve(1),
                2: Promise.resolve(2),
            },
            async element => element * 2
        );
        checkResult(promise);
    });

    test("does not double construct from array like object", () => {
        let callCount = 0;

        class TestArray {
            constructor() {
                callCount += 1;
            }
        }

        let promise = Array.fromAsync.call(TestArray, {
            length: 3,
            0: Promise.resolve(0),
            1: Promise.resolve(2),
            2: Promise.resolve(4),
        });

        checkResult(promise, TestArray);
        expect(callCount).toBe(1);
    });
});
