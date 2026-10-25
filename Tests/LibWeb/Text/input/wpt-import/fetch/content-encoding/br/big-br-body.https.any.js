// META: global=window,worker

const EXPECTED_SIZE = 27000000;
const EXPECTED_SHA256 = [
    74,  100, 37, 243, 147, 61,  116, 60,  241, 221, 126,
    18,  24,  71, 204, 28,  50,  62,  201, 130, 152, 225,
    217, 183, 10, 201, 143, 214, 102, 155, 212, 248,
  ];

promise_test(async () => {
  const response = await fetch('resources/big.text.br');
  assert_true(response.ok);
  const arrayBuffer = await response.arrayBuffer();
  assert_equals(arrayBuffer.byteLength, EXPECTED_SIZE,
               'uncompressed size should match');
  const sha256 = await crypto.subtle.digest('SHA-256', arrayBuffer);
  assert_array_equals(new Uint8Array(sha256), EXPECTED_SHA256,
                      'digest should match');
}, 'large br data should be decompressed successfully');

promise_test(async () => {
  const response = await fetch('resources/big.text.br');
  assert_true(response.ok);
  const reader = response.body.getReader({mode: 'byob'});
  let offset = 0;
  // Pre-allocate space for the output. The response body will be read
  // chunk-by-chunk into this array.
  let ab = new ArrayBuffer(EXPECTED_SIZE);
  while (offset < EXPECTED_SIZE) {
    // To stress the data pipe, we want to use a different size read each
    // time. Unfortunately, JavaScript doesn't have a seeded random number
    // generator, so this creates the possibility of making this test flaky if
    // it doesn't work for some edge cases.
    let size = Math.floor(Math.random() * 65535 + 1);
    if (size + offset > EXPECTED_SIZE) {
      size = EXPECTED_SIZE - offset;
    }
    const u8 = new Uint8Array(ab, offset, size);
    const { value, done } = await reader.read(u8);
    ab = value.buffer;
    // Check that we got our original array back.
    assert_equals(ab.byteLength, EXPECTED_SIZE,
                  'backing array should be the same size');
    assert_equals(offset, value.byteOffset, 'offset should match');
    assert_less_than_equal(value.byteLength, size,
                           'we should not have got more than we asked for');
    offset = value.byteOffset + value.byteLength;
    if (done) break;
  }
  assert_equals(offset, EXPECTED_SIZE,
                'we should have read the whole thing');
  const sha256 = await crypto.subtle.digest('SHA-256', new Uint8Array(ab));
  assert_array_equals(new Uint8Array(sha256), EXPECTED_SHA256,
                      'digest should match');
}, 'large br data should be decompressed successfully with byte stream');
