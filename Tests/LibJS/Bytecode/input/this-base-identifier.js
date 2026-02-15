// Test that GetById and PutById emit base_identifier:this
// when the base is a this expression.

function get_from_this() {
    return this.foo;
}

function set_on_this() {
    this.bar = 1;
}

function chained_this_access() {
    return this.a.b;
}

get_from_this();
set_on_this();
try {
    chained_this_access();
} catch {}
