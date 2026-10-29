import { insertCustomIframe, testSupportScript } from "./helpers.mjs";
import { testGetter } from "../../resources/helpers.mjs";

export default ({ expected }) => {
  promise_setup(() => {
    return insertCustomIframe(`javascript:'${testSupportScript}'`);
  });

  // The javascript: URL iframe inherits its origin from the previous occupant
  // of the iframe, which is about:blank, which in turn inherits from the
  // parent. So, the caller needs to tell us what to expect.

  testGetter(0, expected);
};
