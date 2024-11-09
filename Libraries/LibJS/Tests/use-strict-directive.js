test("valid 'use strict; directive", () => {
    expect(
        (() => {
            "use strict";
            return isStrictMode();
        })()
    ).toBeTrue();
    expect(
        // prettier-ignore
        (() => {
            'use strict';
            return isStrictMode();
        })()
    ).toBeTrue();
});

test("invalid 'use strict; directive", () => {
    expect(
        (() => {
            " use strict ";
            return isStrictMode();
        })()
    ).toBeFalse();
    expect(
        (() => {
            `use strict`;
            return isStrictMode();
        })()
    ).toBeFalse();
    expect(
        (() => {
            "use\
            strict";
            return isStrictMode();
        })()
    ).toBeFalse();
    expect(
        (() => {
            "use\ strict";
            return isStrictMode();
        })()
    ).toBeFalse();
    expect(
        (() => {
            "use \163trict";
            return isStrictMode();
        })()
    ).toBeFalse();
    expect(
        (() => {
            `"use strict"`;
            return isStrictMode();
        })()
    ).toBeFalse();
    expect(
        (() => {
            "use strict" + 1;
            return isStrictMode();
        })()
    ).toBeFalse();
});
