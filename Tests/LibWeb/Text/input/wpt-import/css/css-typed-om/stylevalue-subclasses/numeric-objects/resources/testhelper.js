function assert_numeric_type_equals(type, expectedType) {
  const baseTypes = [
    'length', 'angle', 'time', 'frequency', 'resolution', 'flex', 'percent'
  ];
  for (const baseType of baseTypes) {
    assert_equals(type[baseType], expectedType[baseType], baseType);
  }
  assert_equals(type.percentHint, expectedType.percentHint);
}
