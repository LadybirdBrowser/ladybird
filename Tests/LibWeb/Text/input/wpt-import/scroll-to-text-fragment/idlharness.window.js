// META: script=/resources/WebIDLParser.js
// META: script=/resources/idlharness.js

'use strict';

idl_test(
  ['scroll-to-text-fragment'],
  ['dom', 'html'],
  idl_array => {
    idl_array.add_objects({
      Document: ['document'],
      FragmentDirective: ['document.fragmentDirective'],
    });
  }
);
