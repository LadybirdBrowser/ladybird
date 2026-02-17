// Test that logical OR uses the original lhs operand (not dst)
// for the JumpIf condition, and creates blocks in the correct order
// (rhs_block before end_block).

function blocked(d) {
    for (;;) {
        if (d || d) continue;
        break;
    }
}

blocked(false);
