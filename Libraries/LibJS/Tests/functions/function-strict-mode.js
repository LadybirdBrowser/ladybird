test("non strict-mode by default", () => {
    expect(isStrictMode()).toBeFalse();
});

test("use strict with double quotes", () => {
    "use strict";
    expect(isStrictMode()).toBeTrue();
});

// prettier-ignore
test("use strict with single quotes", () => {
    'use strict';
    expect(isStrictMode()).toBeTrue();
});

// prettier-ignore
test("use strict with backticks does not yield strict mode", () => {
    `use strict`;
    expect(isStrictMode()).toBeFalse();
});

// prettier-ignore
test("use strict with single quotes after statement does not yield strict mode code", () => {
    ;'use strict';
    expect(isStrictMode()).toBeFalse();
});

// prettier-ignore
test("use strict with double quotes after statement does not yield strict mode code", () => {
    ;"use strict";
    expect(isStrictMode()).toBeFalse();
});

test("use strict interrupted by a line continuation does not yield strict mode code", () => {
    "use \
strict";
    expect(isStrictMode()).toBeFalse();
});

test("strict mode propagates down the scope chain", () => {
    "use strict";
    expect(isStrictMode()).toBeTrue();
    (function () {
        expect(isStrictMode()).toBeTrue();
    })();
});

test("strict mode does not propagate up the scope chain", () => {
    expect(isStrictMode()).toBeFalse();
    (function () {
        "use strict";
        expect(isStrictMode()).toBeTrue();
    })();
    expect(isStrictMode()).toBeFalse();
});

test('only the string "use strict" yields strict mode code', () => {
    "use stric";
    expect(isStrictMode()).toBeFalse();
});

test("strict mode does not apply global object to |this|", () => {
    "use strict";
    let functionThis;
    (function () {
        functionThis = this;
    })();
    expect(functionThis).toBeUndefined();
});
