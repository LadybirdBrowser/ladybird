// META: script=/resources/WebIDLParser.js
// META: script=/resources/idlharness.js
// META: script=/resources/testdriver.js
// META: script=/resources/testdriver-vendor.js

// https://www.w3.org/TR/geolocation-API/

window.onload = async () => {
  idl_test(["geolocation"], ["hr-time", "html"], (idl_array) => {
    idl_array.add_objects({
      Geolocation: ["navigator.geolocation"],
    });
  });
};
