test(() => {
  // Truncated sequences
  assert_equals(new TextDecoder().decode(new Uint8Array([0xF0])), "\uFFFD");
  assert_equals(new TextDecoder().decode(new Uint8Array([0xF0, 0x9F])), "\uFFFD");
  assert_equals(new TextDecoder().decode(new Uint8Array([0xF0, 0x9F, 0x92])), "\uFFFD");

  // Errors near end-of-queue
  assert_equals(new TextDecoder().decode(new Uint8Array([0xF0, 0x9F, 0x41])), "\uFFFDA");
  assert_equals(new TextDecoder().decode(new Uint8Array([0xF0, 0x41, 0x42])), "\uFFFDAB");
  assert_equals(new TextDecoder().decode(new Uint8Array([0xF0, 0x41, 0xF0])), "\uFFFDA\uFFFD");
  assert_equals(new TextDecoder().decode(new Uint8Array([0xF0, 0x8F, 0x92])), "\uFFFD\uFFFD\uFFFD");
  assert_equals(new TextDecoder("Big5").decode(new Uint8Array([0x81, 0x40])), "\uFFFD@");
  assert_equals(new TextDecoder("Big5").decode(new Uint8Array([0x81, 0x81])), "\uFFFD");
  assert_equals(new TextDecoder("Big5").decode(new Uint8Array([0x87, 0x87, 0x40])), "\uFFFD@");
}, "TextDecoder end-of-queue handling");

test(() => {
  const decoder = new TextDecoder();
  const big5Decoder = new TextDecoder("Big5");

  assert_equals(decoder.decode(new Uint8Array([0xF0]), { stream: true }), "");
  assert_equals(decoder.decode(), "\uFFFD");

  assert_equals(decoder.decode(new Uint8Array([0xF0]), { stream: true }), "");
  assert_equals(decoder.decode(new Uint8Array([0x9F]), { stream: true }), "");
  assert_equals(decoder.decode(), "\uFFFD");

  assert_equals(decoder.decode(new Uint8Array([0xF0, 0x9F]), { stream: true }), "");
  assert_equals(decoder.decode(new Uint8Array([0x92])), "\uFFFD");

  assert_equals(decoder.decode(new Uint8Array([0xF0, 0x9F]), { stream: true }), "");
  assert_equals(decoder.decode(new Uint8Array([0x41]), { stream: true }), "\uFFFDA");
  assert_equals(decoder.decode(), "");

  assert_equals(decoder.decode(new Uint8Array([0xF0, 0x41, 0x42]), { stream: true }), "\uFFFDAB");
  assert_equals(decoder.decode(), "");

  assert_equals(decoder.decode(new Uint8Array([0xF0, 0x41, 0xF0]), { stream: true }), "\uFFFDA");
  assert_equals(decoder.decode(), "\uFFFD");

  assert_equals(decoder.decode(new Uint8Array([0xF0]), { stream: true }), "");
  assert_equals(decoder.decode(new Uint8Array([0x8F]), { stream: true }), "\uFFFD\uFFFD");
  assert_equals(decoder.decode(new Uint8Array([0x92]), { stream: true }), "\uFFFD");
  assert_equals(decoder.decode(), "");

  assert_equals(decoder.decode(new Uint8Array([0xF0, 0xC2, 0x80, 0x2A]), { stream: true }), "\uFFFD\x80*");
  assert_equals(decoder.decode(), "");

  assert_equals(decoder.decode(new Uint8Array([0xF0]), { stream: true }), "");
  assert_equals(decoder.decode(new Uint8Array([0xC2]), { stream: true }), "\uFFFD");
  assert_equals(decoder.decode(new Uint8Array([0x80]), { stream: true }), "\x80");
  assert_equals(decoder.decode(new Uint8Array([0x2A]), { stream: true }), "*");
  assert_equals(decoder.decode(), "");

  assert_equals(decoder.decode(new Uint8Array([0xF0]), { stream: true }), "");
  assert_equals(decoder.decode(new Uint8Array([0xC2]), { stream: true }), "\uFFFD");
  assert_equals(decoder.decode(new Uint8Array([0x80, 0x2A]), { stream: true }), "\x80*");
  assert_equals(decoder.decode(), "");

  assert_equals(decoder.decode(new Uint8Array([0xF0]), { stream: true }), "");
  assert_equals(decoder.decode(new Uint8Array([0xC2, 0x80, 0x2A]), { stream: true }), "\uFFFD\x80*");
  assert_equals(decoder.decode(), "");

  assert_equals(big5Decoder.decode(new Uint8Array([0x81, 0x40]), { stream: true }), "\uFFFD@");
  assert_equals(big5Decoder.decode(), "");

  assert_equals(big5Decoder.decode(new Uint8Array([0x81]), { stream: true }), "");
  assert_equals(big5Decoder.decode(new Uint8Array([0x40]), { stream: true }), "\uFFFD@");
  assert_equals(big5Decoder.decode(), "");
}, "TextDecoder end-of-queue handling using stream: true");
