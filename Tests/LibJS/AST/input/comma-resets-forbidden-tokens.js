// Comma operator must reset forbidden tokens so that ?? can appear
// after && in a different operand of the same comma expression.
var t = true;
(t && t, t ?? 3);
(t || t, t ?? 3);
(t ?? t, t && 3);
(t ?? t, t || 3);
