test(t => {
    sessionStorage.clear();
    localStorage.clear();

    var key = "name";
    var val = "x".repeat(4 * 1024);

    t.add_cleanup(() => {
        sessionStorage.clear();
        localStorage.clear();
    });

    let indexBefore = 0;
    assert_throws_quotaexceedederror(() => {
        while (true) {
            sessionStorage.setItem("" + key + indexBefore, "" + val + indexBefore);
            indexBefore++;
        }
    }, null, null);

    sessionStorage.clear();

    let indexLocal = 0;
    assert_throws_quotaexceedederror(() => {
        while (true) {
            localStorage.setItem("" + key + indexLocal, "" + val + indexLocal);
            indexLocal++;
        }
    }, null, null);

    let indexAfter = 0;
    assert_throws_quotaexceedederror(() => {
        while (true) {
            sessionStorage.setItem("" + key + indexAfter, "" + val + indexAfter);
            indexAfter++;
        }
    }, null, null);

    assert_greater_than_equal(
        indexAfter,
        Math.floor(indexBefore / 2)
    );
}, "sessionStorage retains comparable quota after localStorage exhaustion");
