"do not use strict";
"no really";
// /\ Valid directives which should not trigger strict mode

test("basic functionality", () => {
    expect(isStrictMode()).toBeFalse();

    (function () {
        expect(isStrictMode()).toBeFalse();
    })();

    (() => {
        expect(isStrictMode()).toBeFalse();
    })();

    (() => {
        expect(isStrictMode()).toBeFalse();
    })();

    function a() {
        expect(isStrictMode()).toBeFalse();
    }

    a();

    eval("expect(isStrictMode()).toBeFalse()");
});

test("functions with strict mode", () => {
    expect(isStrictMode()).toBeFalse();

    function a() {
        "this is allowed trust me";
        "use strict";
        expect(isStrictMode()).toBeTrue();
    }

    a();

    expect(isStrictMode()).toBeFalse();

    (() => {
        "use strict";
        expect(isStrictMode()).toBeTrue();
    })();

    function b() {
        eval("expect(isStrictMode()).toBeFalse()");

        function nested() {
            "use strict";
            eval("expect(isStrictMode()).toBeTrue()");
        }

        nested();

        eval("expect(isStrictMode()).toBeFalse()");
    }

    b();
});
