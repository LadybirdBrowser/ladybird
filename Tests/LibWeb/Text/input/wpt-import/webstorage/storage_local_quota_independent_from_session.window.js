test(t => {
    localStorage.clear();
    sessionStorage.clear();

    var key = "name";
    var val = "x".repeat(4 * 1024);

    t.add_cleanup(() => {
        localStorage.clear();
        sessionStorage.clear();
    });

    let indexBefore = 0;
    assert_throws_quotaexceedederror(() => {
        while (true) {
            localStorage.setItem("" + key + indexBefore, "" + val + indexBefore);
            indexBefore++;
        }
    }, null, null);

    localStorage.clear();

    let indexLocal = 0;
    assert_throws_quotaexceedederror(() => {
        while (true) {
            sessionStorage.setItem("" + key + indexLocal, "" + val + indexLocal);
            indexLocal++;
        }
    }, null, null);

    let indexAfter = 0;
    assert_throws_quotaexceedederror(() => {
        while (true) {
            localStorage.setItem("" + key + indexAfter, "" + val + indexAfter);
            indexAfter++;
        }
    }, null, null);

    assert_greater_than_equal(
        indexAfter,
        Math.floor(indexBefore / 2)
    );
}, "localStorage retains comparable quota after sessionStorage exhaustion");
