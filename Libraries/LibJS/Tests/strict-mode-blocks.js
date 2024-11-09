test("Issue #3641, strict mode should be function- or program-level, not block-level", () => {
    function func() {
        expect(isStrictMode()).toBeFalse();

        // prettier-ignore
        {
            "use strict";
            expect(isStrictMode()).toBeFalse();
        }

        // prettier-ignore
        if (true) {
            "use strict";
            expect(isStrictMode()).toBeFalse();
        }

        // prettier-ignore
        do {
            "use strict";
            expect(isStrictMode()).toBeFalse();
        } while (false);
    }

    func();
});
