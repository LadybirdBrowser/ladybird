test("basic functionality", () => {
    let callHoisted = hoisted();
    function hoisted() {
        return "foo";
    }
    expect(hoisted()).toBe("foo");
    expect(callHoisted).toBe("foo");
});

// First two calls produce a ReferenceError, but the declarations should be hoisted
test("functions are hoisted across non-lexical scopes", () => {
    expect(scopedHoisted).toBeUndefined();
    expect(callScopedHoisted).toBeUndefined();
    {
        var callScopedHoisted = scopedHoisted();
        function scopedHoisted() {
            return "foo";
        }
        expect(scopedHoisted()).toBe("foo");
        expect(callScopedHoisted).toBe("foo");
    }
    expect(scopedHoisted()).toBe("foo");
    expect(callScopedHoisted).toBe("foo");
});

test("functions are not hoisted across lexical scopes", () => {
    const test = () => {
        var iife = (function () {
            return declaredLater();
        })();
        function declaredLater() {
            return "yay";
        }
        return iife;
    };

    expect(() => declaredLater).toThrow(ReferenceError);
    expect(test()).toBe("yay");
});
