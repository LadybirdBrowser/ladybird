function evaluateBoolean(expression, context) {
  let doc = context.ownerDocument || context;
  return doc.evaluate(expression, context, null, XPathResult.BOOLEAN_TYPE, null).booleanValue;
}

function evaluateNumber(expression, context) {
  let doc = context.ownerDocument || context;
  return doc.evaluate(expression, context, null, XPathResult.NUMBER_TYPE, null).numberValue;
}

function evaluateString(expression, context) {
  let doc = context.ownerDocument || context;
  return doc.evaluate(expression, context, null, XPathResult.STRING_TYPE, null).stringValue;
}
