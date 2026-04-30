import lldb


def ak_atomic_summary(valobj, internal_dict):
    value = valobj.GetChildMemberWithName("m_value")
    return value.GetSummary() or value.GetValueAsUnsigned()


def ak_bytestring_summary(valobj, internal_dict):
    impl_ptr = valobj.GetChildMemberWithName("m_impl").GetChildMemberWithName("m_ptr")

    if impl_ptr.GetValueAsUnsigned() == 0:
        return '""'

    impl = impl_ptr.Dereference()
    return ak_string_impl_summary(impl, internal_dict)


def ak_string_impl_summary(valobj, internal_dict):
    length = valobj.GetChildMemberWithName("m_length").GetValueAsUnsigned()
    if length == 0:
        return '""'

    data = valobj.GetChildMemberWithName("m_inline_buffer")
    arr = data.GetPointeeData(0, length).uint8s
    return bytes(arr).decode("utf-8", "replace")


def read_process_memory(process, address, byte_count):
    if address == 0 or byte_count == 0:
        return b""

    error = lldb.SBError()  # type: ignore
    memory = process.ReadMemory(address, byte_count, error)
    if error.Fail():
        return None

    return memory


def ak_distinct_numeric_summary(valobj, internal_dict):
    val = valobj.GetNonSyntheticValue()
    m_value = val.GetChildMemberWithName("m_value")
    return m_value.GetValue() or m_value.GetSummary()


def ak_fixedarray_summary(valobj, internal_dict):
    valobj = valobj.GetNonSyntheticValue()
    size = valobj.GetChildMemberWithName("m_size").GetValueAsUnsigned()
    return f"size={size}"


def ak_hashmap_summary(valobj, internal_dict):
    valobj = valobj.GetNonSyntheticValue()
    table = valobj.GetChildMemberWithName("m_table")
    size = table.GetChildMemberWithName("m_size").GetValueAsUnsigned()
    return f"size={size}"


def ak_nonnullrefptr_summary(valobj, internal_dict):
    valobj = valobj.GetNonSyntheticValue()
    m_ptr = valobj.GetChildMemberWithName("m_ptr")
    ptr_value = m_ptr.GetValueAsUnsigned()

    if ptr_value == 0:
        return "nullptr"

    pointee = m_ptr.Dereference()
    if pointee.IsValid():
        ref_count = pointee.GetChildMemberWithName("m_ref_count")
        if ref_count.IsValid():
            ref_count_val = ref_count.GetValueAsUnsigned()
            return f"{hex(ptr_value)} (ref_count={ref_count_val})"
        return hex(ptr_value)
    return hex(ptr_value)


def ak_optional_summary(valobj, internal_dict):
    valobj = valobj.GetNonSyntheticValue()

    m_has_value = valobj.GetChildMemberWithName("m_has_value")
    if m_has_value and m_has_value.IsValid():
        return "Some" if m_has_value.GetValueAsUnsigned() != 0 else "None"

    m_pointer = valobj.GetChildMemberWithName("m_pointer")
    if m_pointer and m_pointer.IsValid():
        return "Some" if m_pointer.GetValueAsUnsigned() != 0 else "None"

    return None


def ak_ownptr_summary(valobj, internal_dict):
    m_ptr = valobj.GetChildMemberWithName("m_ptr")
    ptr_value = m_ptr.GetValueAsUnsigned()

    if ptr_value == 0:
        return "nullptr"

    return hex(ptr_value)


def ak_refcounted_summary(valobj, internal_dict):
    value = valobj.GetChildMemberWithName("m_ref_count")
    return value.GetSummary() or value.GetValue()


def ak_refptr_summary(valobj, internal_dict):
    valobj = valobj.GetNonSyntheticValue()
    m_ptr = valobj.GetChildMemberWithName("m_ptr")
    ptr_value = m_ptr.GetValueAsUnsigned()

    if ptr_value == 0:
        return "nullptr"

    pointee = m_ptr.Dereference()
    if pointee.IsValid():
        ref_count = pointee.GetChildMemberWithName("m_ref_count")
        if ref_count.IsValid():
            ref_count_val = ref_count.GetValueAsUnsigned()
            return f"{hex(ptr_value)} (ref_count={ref_count_val})"
        return hex(ptr_value)
    return hex(ptr_value)


def ak_singlylinkedlist_summary(valobj, internal_dict):
    valobj = valobj.GetNonSyntheticValue()
    m_head = valobj.GetChildMemberWithName("m_head")

    size = 0
    node = m_head
    while node.GetValueAsUnsigned() != 0:
        size += 1
        next_ptr = node.Dereference().GetChildMemberWithName("next")
        node = next_ptr
        if size > 10000:
            break

    return f"size={size}"


def ak_string_summary(valobj, internal_dict):
    valobj = valobj.GetNonSyntheticValue()
    process = valobj.GetProcess()
    if not process.IsValid():
        return None

    impl = valobj.GetChildMemberWithName("m_impl")
    short_string = impl.GetChildMemberWithName("short_string")
    short_string_tag = short_string.GetChildMemberWithName("byte_count_and_short_string_flag").GetValueAsUnsigned()

    if short_string_tag & 1:
        length = short_string_tag >> 2
        if length == 0:
            return '""'

        storage = short_string.GetChildMemberWithName("storage")
        bytes_start = storage.GetLoadAddress()
        raw_bytes = read_process_memory(process, bytes_start, length)
        if raw_bytes is None:
            return None
        return '"' + raw_bytes.decode("utf-8", "replace") + '"'

    data_ptr = impl.GetChildMemberWithName("data")
    if data_ptr.GetValueAsUnsigned() == 0:
        return '""'

    string_data = data_ptr.Dereference()
    if not string_data.IsValid():
        return None

    if string_data.GetChildMemberWithName("m_substring").GetValueAsUnsigned() != 0:
        return None

    length = string_data.GetChildMemberWithName("m_byte_count").GetValueAsUnsigned()
    if length == 0:
        return '""'

    bytes_start = string_data.GetChildMemberWithName("m_bytes_or_substring_data").GetLoadAddress()
    raw_bytes = read_process_memory(process, bytes_start, length)
    if raw_bytes is None:
        return None

    return '"' + raw_bytes.decode("utf-8", "replace") + '"'


def ak_stringview_summary(valobj, internal_dict):
    length = valobj.GetChildMemberWithName("m_length").GetValueAsUnsigned()
    if length == 0:
        return '""'

    characters = valobj.GetChildMemberWithName("m_characters")
    arr = characters.GetPointeeData(0, length).uint8s
    return '"' + bytes(arr).decode("utf-8", "replace") + '"'


def ak_variant_summary(valobj, internal_dict):
    valobj = valobj.GetNonSyntheticValue()

    index = valobj.GetChildMemberWithName("m_index").GetValueAsUnsigned()
    ty = valobj.GetType()
    current_type = ty.GetTemplateArgumentType(index)

    if current_type.IsValid():
        return f"{current_type.GetName()}"
    return "AK::Variant<?>"


def ak_vector_summary(valobj, internal_dict):
    valobj = valobj.GetNonSyntheticValue()
    m_size = valobj.GetChildMemberWithName("m_size")
    size = m_size.GetValueAsUnsigned()
    return f"size={size}"


class AKArraySyntheticProvider:
    def __init__(self, valobj, internal_dict):
        self.valobj = valobj.GetNonSyntheticValue()
        self.update()

    def update(self):
        valType = self.valobj.GetType()
        self.elem_type = valType.GetTemplateArgumentType(0)
        self.size = valType.GetTemplateArgumentValue(self.valobj.GetTarget(), 1).GetValueAsUnsigned()

        if self.size == 0:
            self.data_ptr = None
            return

        data = self.valobj.GetChildMemberWithName("__data")
        self.data_ptr = data.AddressOf().Cast(self.elem_type.GetPointerType())
        self.elem_size = self.elem_type.GetByteSize()

    def has_children(self):
        return self.size > 0

    def num_children(self):
        return self.size

    def get_child_index(self, name):
        if name.startswith("[") and name.endswith("]"):
            try:
                return int(name[1:-1])
            except (ValueError, TypeError):
                return -1
        return -1

    def get_child_at_index(self, index):
        if index < 0 or index >= self.size or self.data_ptr is None:
            return None

        return self.data_ptr.CreateChildAtOffset(f"[{index}]", index * self.elem_size, self.elem_type)


class AKFixedArraySynthProvider:
    def __init__(self, valobj, internal_dict):
        self.valobj = valobj.GetNonSyntheticValue()
        self.update()

    def update(self):
        self.size = self.valobj.GetChildMemberWithName("m_size").GetValueAsUnsigned()
        self.elements = self.valobj.GetChildMemberWithName("m_elements")
        self.element_type = self.elements.GetType().GetPointeeType()
        self.element_size = self.element_type.GetByteSize()

    def has_children(self):
        return self.size > 0

    def num_children(self):
        return self.size

    def get_child_index(self, name):
        if not name.startswith("[") or not name.endswith("]"):
            return -1
        try:
            return int(name[1:-1])
        except ValueError:
            return -1

    def get_child_at_index(self, index):
        if index < 0 or index >= self.num_children():
            return None
        if self.elements.GetValueAsUnsigned() == 0:
            return None
        return self.elements.CreateChildAtOffset(
            f"[{index}]",
            index * self.element_size,
            self.element_type,
        )


class AKHashMapSynthProvider:
    def __init__(self, valobj, internal_dict):
        self.valobj = valobj.GetNonSyntheticValue()
        self.entries = []
        self.update()

    def update(self):
        self.entries = []

        table = self.valobj.GetChildMemberWithName("m_table")
        buckets = table.GetChildMemberWithName("m_buckets")
        size = table.GetChildMemberWithName("m_size").GetValueAsUnsigned()
        mask = table.GetChildMemberWithName("m_mask").GetValueAsUnsigned()

        if size == 0 or buckets.GetValueAsUnsigned() == 0:
            return

        entry_type = table.GetType().GetTemplateArgumentType(0)
        bucket_type = buckets.GetType().GetPointeeType()
        bucket_size = bucket_type.GetByteSize()
        target = self.valobj.GetTarget()

        for i in range(mask + 1):
            if len(self.entries) >= size:
                break

            bucket = buckets.CreateChildAtOffset(
                f"bucket[{i}]",
                i * bucket_size,
                bucket_type,
            )
            state = bucket.GetChildMemberWithName("state").GetValueAsUnsigned()
            if state == 0:
                continue

            storage = bucket.GetChildMemberWithName("storage")
            entry = bucket.CreateValueFromAddress(
                f"entry[{len(self.entries)}]",
                storage.GetAddress().GetLoadAddress(target),
                entry_type,
            )
            self.entries.append(entry)

    def has_children(self):
        return len(self.entries) > 0

    def num_children(self):
        return len(self.entries)

    def get_child_index(self, name):
        if name.startswith("[") and name.endswith("]"):
            try:
                return int(name[1:-1])
            except ValueError:
                return -1
        return -1

    def get_child_at_index(self, index):
        if index < 0 or index >= len(self.entries):
            return None

        entry = self.entries[index]
        key = entry.GetChildMemberWithName("key")
        val = entry.GetChildMemberWithName("value")

        key_str = key.GetSummary() or key.GetValue() or "?"
        key_str = key_str.strip('"')

        return val.Clone(f"[{key_str}]")


class AKIntrusiveListSyntheticProvider:
    def __init__(self, valobj, internal_dict):
        self.valobj = valobj
        self.update()

    def update(self):
        self.val = self.valobj.GetNonSyntheticValue()
        t = self.val.GetType()
        self.element_type = t.GetTemplateArgumentType(0)

        self.node_member_offset = 0
        for i in range(self.element_type.GetNumberOfFields()):
            field = self.element_type.GetFieldAtIndex(i)
            if "IntrusiveListNode" in field.GetType().GetName():
                self.node_member_offset = field.GetOffsetInBytes()
                break

        self.first = self.val.GetChildMemberWithName("m_storage").GetChildMemberWithName("m_first")

    def _node_to_value(self, node):
        addr = node.GetValueAsUnsigned() - self.node_member_offset
        return self.val.CreateValueFromAddress(None, addr, self.element_type)

    def num_children(self):
        count = 0
        current = self.first
        while current.GetValueAsUnsigned() != 0:
            count += 1
            current = current.GetChildMemberWithName("m_next")
        return count

    def get_child_at_index(self, index):
        current = self.first
        i = 0
        while current.GetValueAsUnsigned() != 0:
            if i == index:
                val = self._node_to_value(current)
                return val.Clone(f"[{i}]")
            current = current.GetChildMemberWithName("m_next")
            i += 1
        return None


class AKSinglyLinkedListSyntheticProvider:
    def __init__(self, valobj, internal_dict):
        self.valobj = valobj
        self.update()

    def update(self):
        self.val = self.valobj.GetNonSyntheticValue()
        self.head = self.val.GetChildMemberWithName("m_head")

    def has_children(self):
        return self.head.GetValueAsUnsigned() != 0

    def num_children(self):
        count = 0
        node = self.head
        while node.GetValueAsUnsigned() != 0:
            count += 1
            node = node.GetChildMemberWithName("next")
        return count

    def get_child_at_index(self, index):
        node = self.head
        i = 0
        while node.GetValueAsUnsigned() != 0:
            if i == index:
                val = node.GetChildMemberWithName("value")
                return val.Clone(f"[{i}]")
            node = node.GetChildMemberWithName("next")
            i += 1
        return None

    def get_child_index(self, name):
        if name.startswith("[") and name.endswith("]"):
            try:
                return int(name[1:-1])
            except (ValueError, TypeError):
                return -1
        return -1


class AKVariantSyntheticProvider:
    def __init__(self, valobj, internal_dict):
        self.valobj = valobj
        self.update()

    def update(self):
        self.val = self.valobj.GetNonSyntheticValue()
        self.index = self.val.GetChildMemberWithName("m_index").GetValueAsUnsigned()
        self.data = self.val.GetChildMemberWithName("m_data")
        self.type = self.val.GetType()
        self.num_types = self.type.GetNumberOfTemplateArguments()

        self.active_type = None
        if self.index < self.num_types:
            self.active_type = self.type.GetTemplateArgumentType(self.index)

    def has_children(self):
        return self.active_type is not None

    def num_children(self):
        return 1 if self.active_type else 0

    def get_child_at_index(self, index):
        if index != 0 or not self.active_type:
            return None

        data_addr = self.data.GetAddress().GetLoadAddress(self.val.GetTarget())

        value = self.val.CreateValueFromAddress("value", data_addr, self.active_type)

        return value.Clone("value")

    def get_child_index(self, name):
        return 0 if name == "value" else -1


class AKVectorSynthProvider:
    def __init__(self, valobj, internal_dict):
        self.valobj = valobj.GetNonSyntheticValue()
        self.update()

    def update(self):
        self.m_size = self.valobj.GetChildMemberWithName("m_size")
        self.size = self.m_size.GetValueAsUnsigned()
        self.elements = []
        self.element_type = None

        if self.size == 0:
            return

        m_metadata = self.valobj.GetChildMemberWithName("m_metadata")
        outline_buffer = m_metadata.GetChildMemberWithName("outline_buffer")

        self.element_type = self.valobj.GetType().GetTemplateArgumentType(0)

        if outline_buffer.GetValueAsUnsigned() != 0:
            elements_ptr = outline_buffer.Cast(self.element_type.GetPointerType())
            self.elements_ptr = elements_ptr
        else:
            inline_buffer = self.valobj.GetChildMemberWithName("m_inline_buffer_storage")
            self.elements_ptr = inline_buffer.Cast(self.element_type.GetPointerType())

    def has_children(self):
        return self.size > 0

    def num_children(self):
        return self.size

    def get_child_index(self, name):
        if name.startswith("[") and name.endswith("]"):
            try:
                return int(name[1:-1])
            except ValueError:
                return -1
        return -1

    def get_child_at_index(self, index):
        if index < 0 or index >= self.size:
            return None
        assert self.element_type is not None
        return self.elements_ptr.CreateChildAtOffset(
            f"[{index}]",
            index * self.element_type.GetByteSize(),
            self.element_type,
        )


def __lldb_init_module(debugger, internal_dict):
    commands = [
        'type synthetic add -x "^AK::Array(<.*>)?$" -l AK.AKArraySyntheticProvider',
        'type summary add -x "^AK::Atomic(<.*>)?$" -F AK.ak_atomic_summary',
        'type summary add -x "^AK::ByteString(<.*>)?$" -F AK.ak_bytestring_summary',
        'type summary add -x "^AK::ByteStringImpl(<.*>)?$" -F AK.ak_string_impl_summary',
        'type summary add -x "^AK::DistinctNumeric(<.*>)?$" -F AK.ak_distinct_numeric_summary',
        'type summary add -x "^AK::FixedArray(<.*>)?$" -F AK.ak_fixedarray_summary',
        'type synthetic add -x "^AK::FixedArray(<.*>)?$" -l AK.AKFixedArraySynthProvider',
        'type summary add -x "^AK::HashMap(<.*>)?$" -F AK.ak_hashmap_summary',
        'type synthetic add -x "^AK::HashMap(<.*>)?$" -l AK.AKHashMapSynthProvider',
        'type synthetic add -x "^AK::IntrusiveList(<.*>)?$" -l AK.AKIntrusiveListSyntheticProvider',
        'type summary add -x "^AK::NonnullRefPtr(<.*>)?$" -F AK.ak_nonnullrefptr_summary',
        'type summary add -x "^AK::Optional(<.*>)?$" -F AK.ak_optional_summary',
        'type summary add -x "^AK::OwnPtr(<.*>)?$" -F AK.ak_ownptr_summary',
        'type summary add -x "^AK::RefCounted(<.*>)?$" -F AK.ak_refcounted_summary',
        'type summary add -x "^AK::RefPtr(<.*>)?$" -F AK.ak_refptr_summary',
        'type summary add -x "^AK::SinglyLinkedList(<.*>)?$" -F AK.ak_singlylinkedlist_summary',
        'type summary add -x "^AK::String(<.*>)?$" -F AK.ak_string_summary',
        'type summary add -x "^AK::StringView(<.*>)?$" -F AK.ak_stringview_summary',
        'type summary add -x "^AK::Variant(<.*>)?$" -F AK.ak_variant_summary',
        'type summary add -x "^AK::Vector(<.*>)?$" -F AK.ak_vector_summary',
        'type synthetic add -x "^AK::Variant(<.*>)?$" -l AK.AKVariantSyntheticProvider',
        'type synthetic add -x "^AK::Vector(<.*>)?$" -l AK.AKVectorSynthProvider',
        'type synthetic add -x "^AK::SinglyLinkedList(<.*>)?$" -l AK.AKSinglyLinkedListSyntheticProvider',
    ]

    for cmd in commands:
        try:
            debugger.HandleCommand(cmd)
        except Exception as e:
            print(f"ak.py: Failed to run command: {cmd}")
            print(f"  Error: {e}")
