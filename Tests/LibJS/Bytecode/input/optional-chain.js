// Test that optional chain expressions produce correct bytecode:
// - Shared short-circuit block for all ?. operations
// - CallWithArgumentArray for calls within chains
// - Correct register reuse (current_value/current_base pattern)

function member(o) {
    return o?.x;
}
function nested_member(o) {
    return o?.x?.y;
}
function call_no_args(o) {
    return o?.foo();
}
function call_with_args(o) {
    return o?.foo(1, 2, 3);
}
function computed(o) {
    return o?.["hello"];
}
function member_then_call(o) {
    return o?.x.foo();
}

member(null);
nested_member(null);
call_no_args(null);
call_with_args(null);
computed(null);
member_then_call(null);
