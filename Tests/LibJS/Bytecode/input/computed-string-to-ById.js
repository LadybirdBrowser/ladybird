// Test that computed member access with string literal keys is optimized
// to GetById/PutNormalById, while array index strings stay as GetByValue.

function get_string_prop(o) {
    return o["hello"];
}
function set_string_prop(o) {
    o["hello"] = 42;
}
function get_index_prop(o) {
    return o["0"];
}
function get_length_prop(o) {
    return o["length"];
}

get_string_prop({});
set_string_prop({});
get_index_prop([]);
get_length_prop([]);
