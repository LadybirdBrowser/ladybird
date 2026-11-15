// META: title=Cookie Store API: cookieStore.delete() return type
// META: global=window,serviceworker

'use strict';

function cookieParamsWithNameAndValueLengths(nameLength, valueLength) {
  return { name: "t".repeat(nameLength), value: "1".repeat(valueLength) }
}

const scenarios = [
  {
    cookie: cookieParamsWithNameAndValueLengths(2048, 2048),
    expected: cookieParamsWithNameAndValueLengths(2048, 2048),
    name: "Set max-size cookie with largest possible name and value (4096 bytes)",
  },
  {
    cookie: cookieParamsWithNameAndValueLengths(4097, 1),
    name: "Ignore cookie with name larger than 4096 and 1 byte value",
  },
  {
    cookie: cookieParamsWithNameAndValueLengths(4096, 0),
    expected: cookieParamsWithNameAndValueLengths(4096, 0),
    name: "Set max-size value-less cookie",
  },
  {
    cookie: cookieParamsWithNameAndValueLengths(4097, 0),
    name: "Ignore value-less cookie with name larger than 4096 bytes",
  },
  {
    cookie: cookieParamsWithNameAndValueLengths(1, 4095),
    expected: cookieParamsWithNameAndValueLengths(1, 4095),
    name: "Set max-size cookie with largest possible value (4095 bytes)",
  },
  {
    cookie: cookieParamsWithNameAndValueLengths(1, 4096),
    name: "Ignore named cookie (with non-zero length) and value larger than 4095 bytes",
  },
  {
    cookie: cookieParamsWithNameAndValueLengths(4096, 1),
    name: "Ignore named cookie with length larger than 4095 bytes, and a non-zero value",
  },
  {
    cookie: cookieParamsWithNameAndValueLengths(0, 4096),
    expected: cookieParamsWithNameAndValueLengths(0, 4096),
    name: "Set max-size name-less cookie",
  },
  {
    cookie: cookieParamsWithNameAndValueLengths(0, 4097),
    name: "Ignore name-less cookie with value larger than 4096 bytes",
  },
  {
    cookie: cookieParamsWithNameAndValueLengths(0, 4097),
    name: "Ignore name-less cookie (without leading =) with value larger than 4096 bytes",
  },
];

for (const scenario of scenarios) {
  promise_test(async testCase => {
    let value = "";
    try {
      await cookieStore.set(scenario.cookie.name, scenario.cookie.value);
      value = (await cookieStore.get(scenario.cookie.name))?.value;
      assert_equals(value, scenario.expected.value);
      await cookieStore.delete({ name: scenario.cookie.name });
    } catch(e) {
      assert_equals(scenario.expected, undefined);
    }
  }, scenario.name);
}
