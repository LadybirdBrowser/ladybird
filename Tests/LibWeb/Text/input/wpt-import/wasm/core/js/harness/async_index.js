/*
 * Copyright 2018 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

"use strict";

let testNum = (function() {
  let count = 1;
  return function() {
    return `#${count++} `;
  };
})();

function uniqueTest(func, desc) {
  test(func, testNum() + desc);
}

// WPT's assert_throw uses a list of predefined, hardcoded known errors. Since
// it is not aware of the WebAssembly error types (yet), implement our own
// version.
function assertThrows(func, err) {
  let caught = false;
  try {
    func();
  } catch (e) {
    assert_true(
      e instanceof err,
      `expected ${err.name}, observed ${e.constructor.name}`
    );
    caught = true;
  }
  assert_true(caught, testNum() + "assertThrows must catch any error.");
}

/******************************************************************************
 ***************************** WAST HARNESS ************************************
 ******************************************************************************/

const EXPECT_INVALID = false;

/* DATA **********************************************************************/

let externrefs = {};
let externsym = Symbol("externref");
function externref(s) {
  if (! (s in externrefs)) externrefs[s] = {[externsym]: s};
  return externrefs[s];
}
function is_externref(x) {
  return (x !== null && externsym in x) ? 1 : 0;
}
function is_funcref(x) {
  return typeof x === "function" ? 1 : 0;
}
function eq_externref(x, y) {
  return x === y ? 1 : 0;
}
function eq_funcref(x, y) {
  return x === y ? 1 : 0;
}

// Default imports.
var registry = {};

// All tests run asynchronously and return their results as promises. To ensure
// that all tests execute in the correct order, we chain the promises together
// so that a test is only executed when all previous tests have finished their
// execution.
let chain = Promise.resolve();

// Resets the registry between two different WPT tests.
function reinitializeRegistry() {
  if (typeof WebAssembly === "undefined") return;

  chain = chain.then(_ => {
    let spectest = {
      externref: externref,
      is_externref: is_externref,
      is_funcref: is_funcref,
      eq_externref: eq_externref,
      eq_funcref: eq_funcref,
      print: console.log.bind(console),
      print_i32: console.log.bind(console),
      print_i64: console.log.bind(console),
      print_i32_f32: console.log.bind(console),
      print_f64_f64: console.log.bind(console),
      print_f32: console.log.bind(console),
      print_f64: console.log.bind(console),
      global_i32: 666,
      global_i64: 666n,
      global_f32: 666.6,
      global_f64: 666.6,
      table: new WebAssembly.Table({
        initial: 10,
        maximum: 20,
        element: "anyfunc"
      }),
      memory: new WebAssembly.Memory({ initial: 1, maximum: 2 })
    };
    let handler = {
      get(target, prop) {
        return prop in target ? target[prop] : {};
      }
    };
    registry = new Proxy({ spectest }, handler);
  });

  // This function is called at the end of every generated js test file. By
  // adding the chain as a promise_test here we make sure that the WPT harness
  // waits for all tests in the chain to finish.
  promise_test(_ => chain, testNum() + "Reinitialize the default imports");
}

reinitializeRegistry();

/* WAST POLYFILL *************************************************************/

function binary(bytes) {
  let buffer = new ArrayBuffer(bytes.length);
  let view = new Uint8Array(buffer);
  for (let i = 0; i < bytes.length; ++i) {
    view[i] = bytes.charCodeAt(i);
  }
  return buffer;
}

/**
 * Returns a compiled module, or throws if there was an error at compilation.
 */
function module(bytes, valid = true) {
  const test = valid
    ? "Test that WebAssembly compilation succeeds"
    : "Test that WebAssembly compilation fails";
  const loc = new Error().stack.toString().replace("Error", "");
  let buffer = binary(bytes);
  let validated = WebAssembly.validate(buffer);

  uniqueTest(_ => {
    assert_equals(valid, validated);
  }, test);

  chain = chain.then(_ => WebAssembly.compile(buffer)).then(
    module => {
      uniqueTest(_ => {
        assert_true(valid, loc);
      }, test);
      return module;
    },
    error => {
      uniqueTest(_ => {
        assert_true(
          !valid,
          `WebAssembly.compile failed unexpectedly with ${error} at {loc}`
        );
      }, test);
    }
  );
  return chain;
}

function assert_invalid(bytes) {
  module(bytes, EXPECT_INVALID);
}

const assert_malformed = assert_invalid;

function instance(bytes, imports, valid = true) {
  const test = valid
    ? "Test that WebAssembly instantiation succeeds"
    : "Test that WebAssembly instantiation fails";
  const loc = new Error().stack.toString().replace("Error", "");
  chain = Promise.all([imports, chain])
    .then(values => {
      let imports = values[0] ? values[0] : registry;
      return WebAssembly.instantiate(binary(bytes), imports);
    })
    .then(
      pair => {
        uniqueTest(_ => {
          assert_true(valid, loc);
        }, test);
        return pair.instance;
      },
      error => {
        uniqueTest(_ => {
          assert_true(
            !valid,
            `unexpected instantiation error, observed ${error} ${loc}`
          );
        }, test);
        return error;
      }
    );
  return chain;
}

function exports(instance) {
  return instance.then(inst => {
    return { module: inst.exports, spectest: registry.spectest };
  });
}

function call(instance, name, args) {
  return Promise.all([instance, chain]).then(values => {
    return values[0].exports[name](...args);
  });
}

function run(action) {
  const test = "Run a WebAssembly test without special assertions";
  const loc = new Error().stack.toString().replace("Error", "");
  chain = Promise.all([chain, action()])
    .then(
      _ => {
        uniqueTest(_ => {}, test);
      },
      error => {
        uniqueTest(_ => {
          assert_true(
            false,
            `unexpected runtime error, observed ${error} ${loc}`
          );
        }, "run");
      }
    )
    // Clear all exceptions, so that subsequent tests get executed.
    .catch(_ => {});
}

function assert_trap(action) {
  const test = "Test that a WebAssembly code traps";
  const loc = new Error().stack.toString().replace("Error", "");
  chain = Promise.all([chain, action()])
    .then(
      result => {
        uniqueTest(_ => {
          assert_true(false, loc);
        }, test);
      },
      error => {
        uniqueTest(_ => {
          assert_true(
            error instanceof WebAssembly.RuntimeError,
            `expected runtime error, observed ${error} ${loc}`
          );
        }, test);
      }
    )
    // Clear all exceptions, so that subsequent tests get executed.
    .catch(_ => {});
}

function assert_return(action, ...expected) {
  const test = "Test that a WebAssembly code returns a specific result";
  const loc = new Error().stack.toString().replace("Error", "");
  chain = Promise.all([action(), chain])
    .then(
      values => {
        uniqueTest(_ => {
          let actual = values[0];
          if (actual === undefined) {
            actual = [];
          } else if (!Array.isArray(actual)) {
            actual = [actual];
          }
          if (actual.length !== expected.length) {
            throw new Error(expected.length + " value(s) expected, got " + actual.length);
          }
          for (let i = 0; i < actual.length; ++i) {
            switch (expected[i]) {
              case "nan:canonical":
              case "nan:arithmetic":
              case "nan:any":
                // Note that JS can't reliably distinguish different NaN values,
                // so there's no good way to test that it's a canonical NaN.
                assert_true(Number.isNaN(actual[i]), `expected NaN, observed ${actual[i]}.`);
                return;
              case "ref.func":
                assert_true(typeof actual[i] === "function", `expected Wasm function, got ${actual[i]}`);
                return;
              case "ref.extern":
                assert_true(actual[i] !== null, `expected Wasm reference, got ${actual[i]}`);
                return;
              default:
                assert_equals(actual[i], expected[i], loc);
            }
          }
        }, test);
      },
      error => {
        uniqueTest(_ => {
          assert_true(
            false,
            `unexpected runtime error, observed ${error} ${loc}`
          );
        }, test);
      }
    )
    // Clear all exceptions, so that subsequent tests get executed.
    .catch(_ => {});
}

let StackOverflow;
try {
  (function f() {
    1 + f();
  })();
} catch (e) {
  StackOverflow = e.constructor;
}

function assert_exhaustion(action) {
  const test = "Test that a WebAssembly code exhauts the stack space";
  const loc = new Error().stack.toString().replace("Error", "");
  chain = Promise.all([action(), chain])
    .then(
      _ => {
        uniqueTest(_ => {
          assert_true(false, loc);
        }, test);
      },
      error => {
        uniqueTest(_ => {
          assert_true(
            error instanceof StackOverflow,
            `expected runtime error, observed ${error} ${loc}`
          );
        }, test);
      }
    )
    // Clear all exceptions, so that subsequent tests get executed.
    .catch(_ => {});
}

function assert_unlinkable(bytes) {
  const test = "Test that a WebAssembly module is unlinkable";
  const loc = new Error().stack.toString().replace("Error", "");
  instance(bytes, registry, EXPECT_INVALID)
    .then(
      result => {
        uniqueTest(_ => {
          assert_true(
            result instanceof WebAssembly.LinkError,
            `expected link error, observed ${result} ${loc}`
          );
        }, test);
      },
      _ => {
        uniqueTest(_ => {
          assert_true(false, loc);
        }, test);
      }
    )
    // Clear all exceptions, so that subsequent tests get executed.
    .catch(_ => {});
}

function assert_uninstantiable(bytes) {
  const test = "Test that a WebAssembly module is uninstantiable";
  const loc = new Error().stack.toString().replace("Error", "");
  instance(bytes, registry, EXPECT_INVALID)
    .then(
      result => {
        uniqueTest(_ => {
          assert_true(
            result instanceof WebAssembly.RuntimeError,
            `expected link error, observed ${result} ${loc}`
          );
        }, test);
      },
      _ => {
        uniqueTest(_ => {
          assert_true(false, loc);
        }, test);
      }
    )
    // Clear all exceptions, so that subsequent tests get executed.
    .catch(_ => {});
}

function register(name, instance) {
  const test =
    "Test that the exports of a WebAssembly module can be registered";
  const loc = new Error().stack.toString().replace("Error", "");
  let stack = new Error();
  chain = Promise.all([instance, chain])
    .then(
      values => {
        registry[name] = values[0].exports;
      },
      _ => {
        uniqueTest(_ => {
          assert_true(false, loc);
        }, test);
      }
    )
    // Clear all exceptions, so that subsequent tests get executed.
    .catch(_ => {});
}

function get(instance, name) {
  const test = "Test that an export of a WebAssembly instance can be acquired";
  const loc = new Error().stack.toString().replace("Error", "");
  chain = Promise.all([instance, chain]).then(
    values => {
      let v = values[0].exports[name];
      return (v instanceof WebAssembly.Global) ? v.value : v;
    },
    _ => {
      uniqueTest(_ => {
        assert_true(false, loc);
      }, test);
    }
  );
  return chain;
}

