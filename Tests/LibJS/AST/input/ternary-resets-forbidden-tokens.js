// The ternary operator must not propagate forbidden tokens from
// the condition into the consequent/alternate branches.
1 && 1 ? 1 : (1 ?? 1);
1 || 1 ? 1 : (1 ?? 1);
(1 ?? 1) ? 1 : 1 && 1;
(1 ?? 1) ? 1 : 1 || 1;
