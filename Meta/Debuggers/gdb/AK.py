# Copyright (c) 2021, Gunnar Beutner <gunnar@beutner.name>
#
# SPDX-License-Identifier: BSD-2-Clause

import re

import gdb  # type: ignore[import-untyped]
import gdb.printing  # type: ignore[import-untyped]
import gdb.types  # type: ignore[import-untyped]

void_ptr = gdb.lookup_type("void").pointer()


def handler_class_for_type(type, re=re.compile("^([^<]+)(<.*>)?$")):
    typename = str(type.tag)

    match = re.match(typename)
    if not match:
        return UnhandledType

    klass = match.group(1)

    if klass == "AK::Array":
        return AKArray
    elif klass == "AK::Atomic":
        return AKAtomic
    elif klass == "AK::Detail::IntrusiveList":
        return AKIntrusiveList
    elif klass == "AK::DistinctNumeric":
        return AKDistinctNumeric
    elif klass == "AK::FixedArray":
        return AKFixedArrayPrinter
    elif klass == "AK::HashMap":
        return AKHashMap
    elif klass == "AK::RefCounted":
        return AKRefCounted
    elif klass == "AK::RefPtr":
        return AKRefPtr
    elif klass == "AK::OwnPtr":
        return AKOwnPtr
    elif klass == "AK::NonnullRefPtr":
        return AKRefPtr
    elif klass == "AK::SinglyLinkedList":
        return AKSinglyLinkedList
    elif klass == "AK::String":
        return AKString
    elif klass == "AK::ByteString":
        return AKByteString
    elif klass == "AK::StringView":
        return AKStringView
    elif klass == "AK::Variant":
        return AKVariant
    elif klass == "AK::Optional":
        return AKOptional
    elif klass == "AK::Vector":
        return AKVector
    else:
        return UnhandledType


class UnhandledType:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        return self.val.type.name

    @classmethod
    def prettyprint_type(cls, type):
        return type.name


class AKAtomic:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        return self.val["m_value"]

    @classmethod
    def prettyprint_type(cls, type):
        contained_type = type.template_argument(0)
        return f"AK::Atomic<{handler_class_for_type(contained_type).prettyprint_type(contained_type)}>"


class AKIntrusiveList:
    def __init__(self, val: gdb.Value):
        self.val = val
        self.element_type = self.val.type.template_argument(0)

        node_member_offset = 0
        for field in self.element_type.fields():
            if "IntrusiveListNode" in str(field.type):
                node_member_offset = (field.bitpos or 0) // 8
                break

        self.node_member_offset = node_member_offset

    def _node_to_value(self, node_ptr: gdb.Value) -> gdb.Value:
        addr = int(node_ptr) - self.node_member_offset
        return gdb.Value(addr).cast(self.element_type.pointer()).dereference()

    def to_string(self):
        return AKIntrusiveList.prettyprint_type(self.val.type)

    def children(self):
        current = self.val["m_storage"]["m_first"]
        i = 0
        while current != 0:
            yield f"[{i}]", self._node_to_value(current)
            current = current["m_next"]
            i += 1

    @classmethod
    def prettyprint_type(cls, type):
        element_type = type.template_argument(0)
        return f"AK::IntrusiveList<{handler_class_for_type(element_type).prettyprint_type(element_type)}>"


class AKDistinctNumeric:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        return self.val["m_value"]

    @classmethod
    def prettyprint_type(cls, type):
        actual_name = type.template_argument(1)
        parts = actual_name.name.split("::")
        unqualified_name = re.sub(r"__(\w+)_tag", r"\1", actual_name.name)
        if unqualified_name != actual_name.name:
            qualified_name = "::".join(parts[:-2] + [unqualified_name])
            return qualified_name
        # If the tag is malformed, just print DistinctNumeric<T>
        contained_type = type.template_argument(0)
        return f"AK::DistinctNumeric<{handler_class_for_type(contained_type).prettyprint_type(contained_type)}>"


class AKFixedArrayPrinter:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        size = self.val["m_size"]
        return f"{AKFixedArrayPrinter.prettyprint_type(self.val.type)} of size {size}"

    def children(self):
        size = self.val["m_size"]
        elements = self.val["m_elements"]

        # Very arbitrary limit, just to catch UAF'd and garbage vector values with a silly number of elements
        if size > 373373:
            return []

        return [(f"[{i}]", elements[i]) for i in range(size)]

    @classmethod
    def prettyprint_type(cls, type):
        template_type = type.template_argument(0)
        return f"AK::FixedArray<{handler_class_for_type(template_type).prettyprint_type(template_type)}>"


class AKRefCounted:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        return self.val["m_ref_count"]

    @classmethod
    def prettyprint_type(cls, type):
        contained_type = type.template_argument(0)
        return f"AK::RefCounted<{handler_class_for_type(contained_type).prettyprint_type(contained_type)}>"


class AKString:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        # Using the internal structure directly is quite convoluted here because of the packing optimizations
        # of AK::String (could be a short string, a substring, or a normal string).
        # This workaround was described in the gdb bugzilla on a discussion of supporting direct method calls
        # on values: https://sourceware.org/bugzilla/show_bug.cgi?id=13326
        gdb.set_convenience_variable("_tmp", self.val.reference_value())
        string_view = gdb.parse_and_eval("$_tmp.bytes_as_string_view()")
        return AKStringView(string_view).to_string()

    @classmethod
    def prettyprint_type(cls, type):
        return "AK::String"


class AKByteString:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        if int(self.val["m_impl"]["m_ptr"]) == 0:
            return '""'
        else:
            impl = AKRefPtr(self.val["m_impl"]).get_pointee().dereference()
            return AKStringImpl(impl).to_string()

    @classmethod
    def prettyprint_type(cls, type):
        return "AK::ByteString"


class AKStringView:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        if int(self.val["m_length"]) == 0:
            return '""'
        else:
            return '"' + self.val["m_characters"].string(length=self.val["m_length"]) + '"'

    @classmethod
    def prettyprint_type(cls, type):
        return "AK::StringView"


def get_field_unalloced(val, member, type):
    # Trying to access a variable-length field seems to fail with
    # Python Exception <class 'gdb.error'> value requires 4294967296 bytes, which is more than max-value-size
    # This works around that issue.
    return gdb.parse_and_eval(f"*({type}*)(({val.type.name}*){int(val.address)})->{member}")


class AKStringImpl:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        if int(self.val["m_length"]) == 0:
            return '""'
        else:
            return self.val["m_inline_buffer"].string(length=self.val["m_length"])

    @classmethod
    def prettyprint_type(cls, type):
        return "AK::StringImpl"


class AKOwnPtr:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        return AKOwnPtr.prettyprint_type(self.val.type)

    def children(self):
        return [("*", self.val["m_ptr"])]

    @classmethod
    def prettyprint_type(cls, type):
        contained_type = type.template_argument(0)
        return f"AK::OwnPtr<{handler_class_for_type(contained_type).prettyprint_type(contained_type)}>"


class AKRefPtr:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        return AKRefPtr.prettyprint_type(self.val.type)

    def get_pointee(self):
        inner_type = self.val.type.template_argument(0)
        inner_type_ptr = inner_type.pointer()
        return self.val["m_ptr"].cast(inner_type_ptr)

    def children(self):
        return [("*", self.get_pointee())]

    @classmethod
    def prettyprint_type(cls, type):
        contained_type = type.template_argument(0)
        return f"AK::RefPtr<{handler_class_for_type(contained_type).prettyprint_type(contained_type)}>"


class AKVariant:
    def __init__(self, val):
        self.val = val
        self.index = int(self.val["m_index"])
        self.contained_types = self.resolve_types(self.val.type)

    def to_string(self):
        return AKVariant.prettyprint_type(self.val.type)

    def children(self):
        data = self.val["m_data"]
        ty = self.contained_types[self.index]
        return [(ty.name, data.cast(ty.pointer()).referenced_value())]

    @classmethod
    def resolve_types(cls, ty):
        contained_types = []
        type_resolved = ty.strip_typedefs()
        index = 0
        while True:
            try:
                arg = type_resolved.template_argument(index)
                index += 1
                contained_types.append(arg)
            except RuntimeError:
                break
        return contained_types

    @classmethod
    def prettyprint_type(cls, ty):
        names = ", ".join(handler_class_for_type(t).prettyprint_type(t) for t in AKVariant.resolve_types(ty))
        return f"AK::Variant<{names}>"


class AKOptional:
    def __init__(self, val):
        self.val = val
        self.has_value = bool(self.val["m_has_value"])
        self.contained_type = self.val.type.strip_typedefs().template_argument(0)

    def to_string(self):
        return AKOptional.prettyprint_type(self.val.type)

    def children(self):
        if self.has_value:
            data = self.val["m_storage"]
            return [(self.contained_type.name, data)]
        return [("OptionalNone", "{}")]

    @classmethod
    def prettyprint_type(cls, type):
        template_type = type.template_argument(0)
        return f"AK::Optional<{template_type}>"


class AKVector:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        return f"{AKVector.prettyprint_type(self.val.type)} of len {int(self.val['m_size'])}"

    def children(self):
        vec_len = int(self.val["m_size"])

        if vec_len == 0:
            return []

        outline_buf = self.val["m_metadata"]["outline_buffer"]

        inner_type_ptr = self.val.type.template_argument(0).pointer()

        if int(outline_buf) != 0:
            elements = outline_buf.cast(inner_type_ptr)
        else:
            elements = get_field_unalloced(self.val, "m_inline_buffer_storage", inner_type_ptr)

        return [(f"[{i}]", elements[i]) for i in range(vec_len)]

    @classmethod
    def prettyprint_type(cls, type):
        template_type = type.template_argument(0)
        return f"AK::Vector<{handler_class_for_type(template_type).prettyprint_type(template_type)}>"


class AKArray:
    def __init__(self, val):
        self.val = val
        self.storage_type = self.val.type.template_argument(0)
        self.array_size = self.val.type.template_argument(1)

    def to_string(self):
        return AKArray.prettyprint_type(self.val.type)

    def children(self):
        data_array = self.val["__data"]
        storage_type_ptr = self.storage_type.pointer()
        elements = data_array.cast(storage_type_ptr)

        return [(f"[{i}]", elements[i]) for i in range(self.array_size)]

    @classmethod
    def prettyprint_type(cls, type):
        template_type = type.template_argument(0)
        template_size = type.template_argument(1)
        return f"AK::Array<{template_type}, {template_size}>"


class AKHashMap:
    def __init__(self, val):
        self.val = val

    @staticmethod
    def _iter_hashtable(val, cb):
        entry_type_ptr = val.type.template_argument(0).pointer()
        buckets = val["m_buckets"]
        for i in range(0, int(val["m_mask"]) + 1):
            bucket = buckets[i]
            # if state == Used
            if int(bucket["state"]) != 0:
                cb(bucket["storage"].cast(entry_type_ptr))

    def display_hint(self):
        return "map"

    @staticmethod
    def _iter_hashmap(val, cb):
        table = val["m_table"]
        AKHashMap._iter_hashtable(table, lambda entry: cb(entry["key"], entry["value"]))

    def to_string(self):
        return AKHashMap.prettyprint_type(self.val.type)

    def children(self):
        elements = []

        def cb(key, value):
            index = len(elements) // 2
            elements.append((f"key[{index}]", key))
            elements.append((f"value[{index}]", value))

        AKHashMap._iter_hashmap(self.val, cb)
        return elements

    @classmethod
    def prettyprint_type(cls, type):
        template_types = list(type.template_argument(i) for i in (0, 1))
        key, value = list(handler_class_for_type(t).prettyprint_type(t) for t in template_types)
        return f"AK::HashMap<{key}, {value}>"


class AKSinglyLinkedList:
    def __init__(self, val):
        self.val = val

    def to_string(self):
        return AKSinglyLinkedList.prettyprint_type(self.val.type)

    def children(self):
        elements = []

        node = self.val["m_head"]
        while node != 0:
            elements.append(node["value"])
            node = node["next"]

        return [(f"[{i}]", elements[i]) for i in range(len(elements))]

    @classmethod
    def prettyprint_type(cls, type):
        template_type = type.template_argument(0)
        return f"AK::SinglyLinkedList<{handler_class_for_type(template_type).prettyprint_type(template_type)}>"


class LadybirdPrettyPrinterLocator(gdb.printing.PrettyPrinter):
    def __init__(self):
        super(LadybirdPrettyPrinterLocator, self).__init__("ladybird_pretty_printers", [])

    def __call__(self, val):  # type: ignore[override]
        type = gdb.types.get_basic_type(val.type)
        handler = handler_class_for_type(type)
        if handler is UnhandledType:
            return None
        return handler(val)  # type: ignore[arg-type]


gdb.printing.register_pretty_printer(None, LadybirdPrettyPrinterLocator(), replace=True)
