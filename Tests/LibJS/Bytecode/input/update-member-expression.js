// Test that update expressions on member expressions put
// base_identifier on the GetById (not the PutNormalById),
// matching the C++ codegen.

function postfix_increment(obj) {
    obj.count++;
}

function prefix_decrement(obj) {
    --obj.count;
}

postfix_increment({});
prefix_decrement({});
