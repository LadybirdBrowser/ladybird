// META: title=Indexed DB and Structured Serializing/Deserializing
// META: timeout=long
// META: script=resources/support-promises.js
// META: script=/common/subset-tests.js
// META: variant=?1-20
// META: variant=?21-40
// META: variant=?41-60
// META: variant=?61-80
// META: variant=?81-100
// META: variant=?101-last

// Tests Indexed DB coverage of HTML's Safe "passing of structured data"
// https://html.spec.whatwg.org/multipage/structured-data.html

function describe(value) {
  let type, str;
  if (typeof value === 'object' && value) {
    type = Object.getPrototypeOf(value).constructor.name;
    // Handle Number(-0), etc.
    str = Object.is(value.valueOf(), -0) ? '-0' : String(value);
  } else {
    type = typeof value;
    // Handle primitive -0.
    str = Object.is(value, -0) ? '-0' : String(value);
  }
  return `${type}: ${str}`;
}

function cloneTest(value, verifyFunc) {
  subsetTest(promise_test, async t => {
    const db = await createDatabase(t, db => {
      const store = db.createObjectStore('store');
      // This index is not used, but evaluating key path on each put()
      // call will exercise (de)serialization.
      store.createIndex('index', 'dummyKeyPath');
    });
    t.add_cleanup(() => {
      if (db) {
        db.close();
        indexedDB.deleteDatabase(db.name);
      }
    });
    const tx = db.transaction('store', 'readwrite');
    const store = tx.objectStore('store');
    await promiseForRequest(t, store.put(value, 'key'));
    const result = await promiseForRequest(t, store.get('key'));
    // Because the async verifyFunc may await async values that are independent
    // of the transaction lifetime (ex: blob.text()), we must only await it
    // after adding listeners to the transaction.
    await promiseForTransaction(t, tx);
    await verifyFunc(value, result);
  }, describe(value));
}

// Specialization of cloneTest() for objects, with common asserts.
function cloneObjectTest(value, verifyFunc) {
  cloneTest(value, async (orig, clone) => {
    assert_not_equals(orig, clone);
    assert_equals(typeof clone, 'object');
    assert_equals(Object.getPrototypeOf(orig), Object.getPrototypeOf(clone));
    await verifyFunc(orig, clone);
  });
}

function cloneFailureTest(value) {
  subsetTest(promise_test, async t => {
    const db = await createDatabase(t, db => {
      db.createObjectStore('store');
    });
    t.add_cleanup(() => {
      if (db) {
        db.close();
        indexedDB.deleteDatabase(db.name);
      }
    });
    const tx = db.transaction('store', 'readwrite');
    const store = tx.objectStore('store');
    assert_throws_dom('DataCloneError', () => store.put(value, 'key'));
  }, 'Not serializable: ' + describe(value));
}

//
// ECMAScript types
//

// Primitive values: Undefined, Null, Boolean, Number, BigInt, String
const booleans = [false, true];
const numbers = [
  NaN,
  -Infinity,
  -Number.MAX_VALUE,
  -0xffffffff,
  -0x80000000,
  -0x7fffffff,
  -1,
  -Number.MIN_VALUE,
  -0,
  0,
  1,
  Number.MIN_VALUE,
  0x7fffffff,
  0x80000000,
  0xffffffff,
  Number.MAX_VALUE,
  Infinity,
];
const bigints = [
  -12345678901234567890n,
  -1n,
  0n,
  1n,
  12345678901234567890n,
];
const strings = [
  '',
  'this is a sample string',
  'null(\0)',
];

[undefined, null].concat(booleans, numbers, bigints, strings)
  .forEach(value => cloneTest(value, (orig, clone) => {
    assert_equals(orig, clone);
  }));

// "Primitive" Objects (Boolean, Number, BigInt, String)
[].concat(booleans, numbers, bigints, strings)
  .forEach(value => cloneObjectTest(Object(value), (orig, clone) => {
    assert_equals(orig.valueOf(), clone.valueOf());
  }));

// Dates
[
  new Date(-1e13),
  new Date(-1e12),
  new Date(-1e9),
  new Date(-1e6),
  new Date(-1e3),
  new Date(0),
  new Date(1e3),
  new Date(1e6),
  new Date(1e9),
  new Date(1e12),
  new Date(1e13)
].forEach(value => cloneTest(value, (orig, clone) => {
    assert_not_equals(orig, clone);
    assert_equals(typeof clone, 'object');
    assert_equals(Object.getPrototypeOf(orig), Object.getPrototypeOf(clone));
    assert_equals(orig.valueOf(), clone.valueOf());
  }));

// Regular Expressions
[
  new RegExp(),
  /abc/,
  /abc/g,
  /abc/i,
  /abc/gi,
  /abc/m,
  /abc/mg,
  /abc/mi,
  /abc/mgi,
  /abc/gimsuy,
].forEach(value => cloneObjectTest(value, (orig, clone) => {
  assert_equals(orig.toString(), clone.toString());
}));

// ArrayBuffer
cloneObjectTest(new Uint8Array([0, 1, 254, 255]).buffer, (orig, clone) => {
  assert_array_equals(new Uint8Array(orig), new Uint8Array(clone));
});

// TODO SharedArrayBuffer

// Array Buffer Views
let byteArrays = [
  new Uint8Array([]),
  new Uint8Array([0, 1, 254, 255]),
  new Uint16Array([0x0000, 0x0001, 0xFFFE, 0xFFFF]),
  new Uint32Array([0x00000000, 0x00000001, 0xFFFFFFFE, 0xFFFFFFFF]),
  new Int8Array([0, 1, 254, 255]),
  new Int16Array([0x0000, 0x0001, 0xFFFE, 0xFFFF]),
  new Int32Array([0x00000000, 0x00000001, 0xFFFFFFFE, 0xFFFFFFFF]),
  new Uint8ClampedArray([0, 1, 254, 255]),
  new Float32Array([-Infinity, -1.5, -1, -0.5, 0, 0.5, 1, 1.5, Infinity, NaN]),
  new Float64Array([-Infinity, -Number.MAX_VALUE, -Number.MIN_VALUE, 0,
                    Number.MIN_VALUE, Number.MAX_VALUE, Infinity, NaN])
]

if (typeof Float16Array !== 'undefined') {
  byteArrays.push(
      new Float16Array([-Infinity, -1.5, -1, -0.5, 0, 0.5, 1, 1.5, Infinity, NaN]));
}

byteArrays.forEach(value => cloneObjectTest(value, (orig, clone) => {
  assert_array_equals(orig, clone);
}));

// Map
cloneObjectTest(new Map([[1,2],[3,4]]), (orig, clone) => {
  assert_array_equals([...orig.keys()], [...clone.keys()]);
  assert_array_equals([...orig.values()], [...clone.values()]);
});

// Set
cloneObjectTest(new Set([1,2,3,4]), (orig, clone) => {
  assert_array_equals([...orig.values()], [...clone.values()]);
});

// Error
[
  new Error(),
  new Error('abc', 'def'),
  new EvalError(),
  new EvalError('ghi', 'jkl'),
  new RangeError(),
  new RangeError('ghi', 'jkl'),
  new ReferenceError(),
  new ReferenceError('ghi', 'jkl'),
  new SyntaxError(),
  new SyntaxError('ghi', 'jkl'),
  new TypeError(),
  new TypeError('ghi', 'jkl'),
  new URIError(),
  new URIError('ghi', 'jkl'),
].forEach(value => cloneObjectTest(value, (orig, clone) => {
  assert_equals(orig.name, clone.name);
  assert_equals(orig.message, clone.message);
}));

// Arrays
[
  [],
  [1,2,3],
  Object.assign(
    ['foo', 'bar'],
    {10: true, 11: false, 20: 123, 21: 456, 30: null}),
  Object.assign(
    ['foo', 'bar'],
    {a: true, b: false, foo: 123, bar: 456, '': null}),
].forEach(value => cloneObjectTest(value, (orig, clone) => {
  assert_array_equals(orig, clone);
  assert_array_equals(Object.keys(orig), Object.keys(clone));
  Object.keys(orig).forEach(key => {
    assert_equals(orig[key], clone[key], `Property ${key}`);
  });
}));

// Objects
cloneObjectTest({foo: true, bar: false}, (orig, clone) => {
  assert_array_equals(Object.keys(orig), Object.keys(clone));
  Object.keys(orig).forEach(key => {
    assert_equals(orig[key], clone[key], `Property ${key}`);
  });
});

//
// [Serializable] Platform objects
//

// TODO: Test these additional interfaces:
// * DOMQuad
// * DOMException
// * RTCCertificate

// Geometry types
[
  new DOMMatrix(),
  new DOMMatrixReadOnly(),
  new DOMPoint(),
  new DOMPointReadOnly(),
  new DOMRect,
  new DOMRectReadOnly(),
].forEach(value => cloneObjectTest(value, (orig, clone) => {
  Object.keys(Object.getPrototypeOf(orig)).forEach(key => {
    assert_equals(orig[key], clone[key], `Property ${key}`);
  });
}));

// ImageData
const image_data = new ImageData(8, 8);
for (let i = 0; i < 256; ++i) {
  image_data.data[i] = i;
}
cloneObjectTest(image_data, (orig, clone) => {
  assert_equals(orig.width, clone.width);
  assert_equals(orig.height, clone.height);
  assert_array_equals(orig.data, clone.data);
});

// Blob
cloneObjectTest(
  new Blob(['This is a test.'], {type: 'a/b'}),
  async (orig, clone) => {
    assert_equals(orig.size, clone.size);
    assert_equals(orig.type, clone.type);
    assert_equals(await orig.text(), await clone.text());
  });

// File
cloneObjectTest(
  new File(['This is a test.'], 'foo.txt', {type: 'c/d'}),
  async (orig, clone) => {
    assert_equals(orig.size, clone.size);
    assert_equals(orig.type, clone.type);
    assert_equals(orig.name, clone.name);
    assert_equals(orig.lastModified, clone.lastModified);
    assert_equals(await orig.text(), await clone.text());
  });


// FileList - exposed in Workers, but not constructable.
if ('document' in self) {
  // TODO: Test with populated list.
  cloneObjectTest(
    Object.assign(document.createElement('input'),
                  {type: 'file', multiple: true}).files,
    async (orig, clone) => {
      assert_equals(orig.length, clone.length);
    });
}

//
// Non-serializable types
//
[
  // ECMAScript types
  function() {},
  Symbol('desc'),

  // Non-[Serializable] platform objects
  self,
  new Event(''),
  new MessageChannel()
].forEach(cloneFailureTest);
