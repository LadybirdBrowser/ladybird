// CoverInitializedName ({a = 0}) is valid when the containing object is
// reinterpreted as a destructuring pattern, but should be rejected when
// the object is used in expression context (e.g. as a member access base).
// This test verifies the valid case parses correctly.
function fn() {
    [{ a = 0 }] = [];
    [{ a: 0 }.x] = [];
}
