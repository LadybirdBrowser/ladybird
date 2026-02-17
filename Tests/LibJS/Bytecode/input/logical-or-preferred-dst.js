// Test that logical OR passes outer preferred_dst (not internal dst)
// to LHS, so a Mov from lhs to dst is emitted before the JumpIf.

function blocked(d) {
    for (;;) {
        if (d || d > 0) continue;
        break;
    }
}

blocked(false);
