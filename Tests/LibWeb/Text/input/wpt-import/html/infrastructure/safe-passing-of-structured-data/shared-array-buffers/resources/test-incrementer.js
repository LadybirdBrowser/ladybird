"use strict";

self.getViewValue = (view, index) => {
  if(view instanceof DataView) {
    return view.getInt8(index);
  }
  return view[index];
};

self.setViewValue = (view, index, value) => {
  if(view instanceof DataView) {
    view.setInt8(index, value);
  } else {
    view[index] = value;
  }
};

self.maybeBigInt = (view, value) => {
  if (view.constructor.name === "BigInt64Array" || view.constructor.name === "BigUint64Array") {
    return BigInt(value);
  }
  return value;
};

self.testSharingViaIncrementerScript = (t, whereToListen, whereToListenLabel, whereToSend, whereToSendLabel, origin, type = "Int32Array") => {
  return new Promise(resolve => {
    const sab = new SharedArrayBuffer(8);
    const view = new self[type](sab);
    setViewValue(view, 0, maybeBigInt(view, 1));

    whereToListen.onmessage = t.step_func(({ data }) => {
      switch (data.message) {
        case "initial payload received": {
          assert_equals(data.value, maybeBigInt(view, 1), `The ${whereToSendLabel} must see the same value in the SharedArrayBuffer`);
          break;
        }

        case "changed to 2": {
          assert_equals(getViewValue(view, 0), maybeBigInt(view, 2), `The ${whereToListenLabel} must see changes made in the ${whereToSendLabel}`);

          setViewValue(view, 0, maybeBigInt(view, 3));
          whereToSend.postMessage({ message: "changed to 3" }, origin);

          break;
        }

        case "changed to 3 received": {
          assert_equals(data.value, maybeBigInt(view, 3), `The ${whereToSendLabel} must see changes made in the ${whereToListenLabel}`);
          resolve();
          break;
        }
      }
    });

    whereToSend.postMessage({ message: "initial payload", view }, origin);
  });
};

self.setupDestinationIncrementer = (whereToListen, whereToSendBackTo, origin) => {
  let view;
  whereToListen.onmessage = ({ data }) => {
    switch (data.message) {
      case "initial payload": {
        view = data.view;
        whereToSendBackTo.postMessage({ message: "initial payload received", value: getViewValue(view, 0) }, origin);

        setViewValue(view, 0, maybeBigInt(view, 2));
        whereToSendBackTo.postMessage({ message: "changed to 2" }, origin);

        break;
      }

      case "changed to 3": {
        whereToSendBackTo.postMessage({ message: "changed to 3 received", value: getViewValue(view, 0) }, origin);

        break;
      }
    }
  };
};
