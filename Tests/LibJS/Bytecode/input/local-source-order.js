// Local variables should be allocated indices in source order, not in
// alphabetical order. With names that sort in reverse alphabetical
// order, the difference is visible in the bytecode dump (~0 first, etc).

function source_order_locals() {
    let zebra = 1;
    let yak = 2;
    let aardvark = 3;
    return zebra + yak + aardvark;
}

source_order_locals();
