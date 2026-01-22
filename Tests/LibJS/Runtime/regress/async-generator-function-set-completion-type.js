test("test", () => {
    var obj = {
        [Symbol.iterator]() {
            return {
                next() {
                    return { value: 1, done: false };
                },
            };
        },
    };

    async function* asyncg() {
        yield* obj;
    }

    let fail = false;
    let done = false;

    function FAIL() {
        fail = true;
    }

    function DONE() {
        done = true;
    }

    var iter = asyncg();

    iter.next()
        .then(function (result) {
            iter.return()
                .then(function (result) {
                    expect(result.done).toBeTrue();
                    expect(result.value).toBeUndefined();
                })
                .catch(FAIL);
        })
        .catch(FAIL)
        .then(DONE);
    runQueuedPromiseJobs();
    expect(fail).toBe(false);
    expect(done).toBe(true);
});
