"use strict";

test("basic functionality", () => {
    expect(isStrictMode()).toBeTrue();

    (function () {
        expect(isStrictMode()).toBeTrue();
    })();

    (() => {
        expect(isStrictMode()).toBeTrue();
    })();

    (() => {
        "use strict";
        expect(isStrictMode()).toBeTrue();
    })();

    function a() {
        expect(isStrictMode()).toBeTrue();
    }

    a();

    eval("expect(isStrictMode()).toBeTrue()");
});
