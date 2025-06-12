function assert_transfer_error(transferList) {
  assert_throws_dom("DataCloneError", () => self.postMessage({ get whatever() { throw new Error("You should not have gotten to this point") } }, "*", transferList));
}

test(() => {
  [self, self.document, new Image()].forEach(val => {
    assert_transfer_error([val]);
  });
}, "Cannot transfer all objects");

function transfer_tests(name, create) {
  promise_test(async () => {
    const transferable = await create();
    assert_transfer_error([transferable, transferable]);
  }, `Cannot transfer the same ${name} twice`);

  promise_test(async () => {
    const transferable = await create();
    self.postMessage(null, "*", [transferable]);
    assert_throws_dom("DataCloneError", () => self.postMessage(null, "*", [transferable]));
  }, `Serialize should make the ${name} detached, so it cannot be transferred again`);

  promise_test(async () => {
    const transferable = await create(),
          customError = new Error("hi");
    self.postMessage(null, "*", [transferable]);
    assert_throws_exactly(customError, () => self.postMessage({ get whatever() { throw customError } }, "*", [transferable]));
  }, `Serialize should throw before a detached ${name} is found`);

  promise_test(async () => {
    const transferable = await create();
    let seen = false;
    const message = {
      get a() {
        self.postMessage(null, '*', [transferable]);
        seen = true;
      }
    };
    assert_throws_dom("DataCloneError", () => self.postMessage(message, "*", [transferable]));
    assert_true(seen);
  }, `Cannot transfer ${name} detached while the message was serialized`);
}

transfer_tests("ArrayBuffer", () => new ArrayBuffer(1));
transfer_tests("MessagePort", () => new MessageChannel().port1);
transfer_tests("ImageBitmap", () => self.createImageBitmap(document.createElement("canvas")));
transfer_tests("OffscreenCanvas", () => new OffscreenCanvas(1, 1));
