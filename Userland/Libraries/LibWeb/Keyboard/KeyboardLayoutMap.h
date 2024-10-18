#pragma once

#include "AK/Vector.h"
#include "LibJS/Runtime/Array.h"
#include "LibJS/Runtime/NumberObject.h"
#include "LibWeb/HTML/DOMStringList.h"
#include "LibWeb/UIEvents/KeyCode.h"
#include <AK/String.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/GCPtr.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Keyboard {

class KeyboardLayoutMap;
}
