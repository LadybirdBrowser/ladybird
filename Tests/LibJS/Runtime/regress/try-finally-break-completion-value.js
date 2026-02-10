// Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
// SPDX-License-Identifier: BSD-2-Clause

test("break in finally with value before break uses that value", () => {
    expect(eval("do { try { 39 } catch (e) { -1 } finally { 42; break; -2 }; } while (false);")).toBe(42);
});

test("break in finally with no value before break produces undefined", () => {
    expect(eval("do { try { 39 } catch (e) { -1 } finally { break; -2 }; } while (false);")).toBeUndefined();
});

test("continue in finally with value before continue uses that value", () => {
    expect(eval("do { try { 39 } catch (e) { -1 } finally { 42; continue; -2 }; } while (false);")).toBe(42);
});

test("break in finally with no value after catch exception produces undefined", () => {
    expect(eval("do { try { [].x.x } catch (e) { -1; } finally { break; -3 }; } while (false);")).toBeUndefined();
});

test("break in finally with value after catch exception uses that value", () => {
    expect(eval("do { try { [].x.x } catch (e) { -1; } finally { 42; break; -3 }; } while (false);")).toBe(42);
});

test("normal finally does not override try completion value", () => {
    expect(eval("do { try { 39 } catch (e) { -1 } finally { 42; }; } while (false);")).toBe(39);
});

test("break in catch with no value before break produces undefined", () => {
    var r = eval(
        "for (var i = 0; i < 2; ++i) { if (i) { try { throw null; } catch (e) { break; } } 'bad completion'; }"
    );
    expect(r).toBeUndefined();
});

test("continue in catch with no value before continue produces undefined", () => {
    var r = eval(
        "for (var i = 0; i < 2; ++i) { if (i) { try { throw null; } catch (e) { continue; } } 'bad completion'; }"
    );
    expect(r).toBeUndefined();
});

test("break in try body with no value before break produces undefined", () => {
    var r = eval('for (var i = 0; i < 2; ++i) { if (i) { try { break; } catch (e) {} } "bad completion"; }');
    expect(r).toBeUndefined();
});
