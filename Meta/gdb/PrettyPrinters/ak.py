"""
GDB pretty-printers for AK types.
"""

import gdb
import gdb.printing


class GenericPrinter:
    def __init__(self, val):
        self.val = val

    def yield_children(self, ignored_fields):
        for field in self.val.type.fields():
            if field.name in ignored_fields:
                continue
            if field.is_base_class:
                yield field.name, self.val.cast(field.type)
            elif field.name is None:
                yield "(anonymous)", self.val[field]
            else:
                yield field.name, self.val[field]

    def children(self):
        try:
            for child in self.yield_children(ignored_fields=[]):
                yield child
        except gdb.MemoryError:
            yield "<error>", "invalid memory"
        except Exception as e:
            yield "<error>", str(e)


def format_string_bytes(data):
    """Format bytes as an escaped string for display"""
    try:
        text = data.decode("utf-8")
        escaped = text.replace("\\", "\\\\").replace('"', '\\"')
        escaped = escaped.replace("\n", "\\n").replace("\r", "\\r").replace("\t", "\\t")
        return f'"{escaped}"'
    except UnicodeDecodeError:
        return f'"{data!r}"'


class AKStringDataPrinter(GenericPrinter):
    """Pretty-printer for AK::Detail::StringData"""

    def __init__(self, val):
        super().__init__(val)

    def get_bytes(self):
        """Extract the string bytes from StringData"""
        byte_count = int(self.val["m_byte_count"])
        is_substring = bool(self.val["m_substring"])

        if byte_count == 0:
            return b""

        if is_substring:
            substring_data_addr = self.val["m_bytes_or_substring_data"].address
            substring_data_type = gdb.lookup_type("AK::Detail::StringData::SubstringData")
            substring_data = substring_data_addr.cast(substring_data_type.pointer()).dereference()

            superstring = substring_data["superstring"]
            start_offset = int(substring_data["start_offset"])

            super_byte_count = int(superstring["m_byte_count"])
            super_is_substring = bool(superstring["m_substring"])

            if super_is_substring:
                return None

            super_bytes_addr = superstring["m_bytes_or_substring_data"].address
            inferior = gdb.selected_inferior()
            all_bytes = inferior.read_memory(super_bytes_addr, super_byte_count)
            return bytes(all_bytes)[start_offset : start_offset + byte_count]

        bytes_addr = self.val["m_bytes_or_substring_data"].address
        inferior = gdb.selected_inferior()
        memory = inferior.read_memory(bytes_addr, byte_count)
        return bytes(memory)

    def display_hint(self):
        return "string"

    def to_string(self):
        try:
            data = self.get_bytes()
            if data is None:
                return "<nested substring>"
            return format_string_bytes(data)
        except gdb.MemoryError:
            return "<invalid memory>"
        except Exception as e:
            return f"<error: {e}>"

    def children(self):
        """Yield (name, value) pairs for StringData fields"""
        try:
            for child in super().yield_children(ignored_fields=["m_bytes_or_substring_data"]):
                yield child

            # m_bytes_or_substring_data is a flexible array member (u8[0])
            data_addr = self.val["m_bytes_or_substring_data"].address
            is_substring = bool(self.val["m_substring"])

            if is_substring:
                substring_data_type = gdb.lookup_type("AK::Detail::StringData::SubstringData")
                yield "m_bytes_or_substring_data", data_addr.cast(substring_data_type.pointer()).dereference()
            else:
                byte_count = int(self.val["m_byte_count"])
                u8_type = gdb.lookup_type("u8")
                u8_array_type = u8_type.array(byte_count - 1) if byte_count > 0 else u8_type.array(0)
                yield "m_bytes_or_substring_data", data_addr.cast(u8_array_type.pointer()).dereference()

        except gdb.MemoryError:
            yield "<error>", "invalid memory"
        except Exception as e:
            yield "<error>", str(e)


class AKStringPrinter(GenericPrinter):
    """Pretty-printer for AK::String and AK::Detail::StringBase"""

    SHORT_STRING_FLAG = 1
    SHORT_STRING_BYTE_COUNT_SHIFT = 2
    MAX_SHORT_STRING_BYTE_COUNT = 7  # sizeof(void*) - 1 on 64-bit

    def __init__(self, val):
        super().__init__(val)

    def _get_impl(self):
        """Get the m_impl union, handling both String and StringBase"""
        if self.val.type.strip_typedefs().name == "AK::String":
            # String inherits from StringBase, access through base class
            string_base = self.val.cast(gdb.lookup_type("AK::Detail::StringBase"))
            return string_base["m_impl"]
        return self.val["m_impl"]

    def _is_short_string(self):
        """Check if this is a short string (inline storage)"""
        impl = self._get_impl()
        short_string = impl["short_string"]
        flag_byte = int(short_string["byte_count_and_short_string_flag"])
        return (flag_byte & self.SHORT_STRING_FLAG) != 0

    def _get_short_string_bytes(self):
        """Extract bytes from a short string"""
        impl = self._get_impl()
        short_string = impl["short_string"]
        flag_byte = int(short_string["byte_count_and_short_string_flag"])
        byte_count = flag_byte >> self.SHORT_STRING_BYTE_COUNT_SHIFT

        if byte_count == 0:
            return b""

        storage = short_string["storage"]
        result = bytearray()
        for i in range(byte_count):
            result.append(int(storage[i]))
        return bytes(result)

    def _get_long_string_bytes(self):
        """Extract bytes from a heap-allocated string (StringData)"""
        impl = self._get_impl()
        data_ptr = impl["data"]

        if int(data_ptr) == 0:
            return b""

        string_data_printer = AKStringDataPrinter(data_ptr.dereference())
        return string_data_printer.get_bytes()

    def display_hint(self):
        return "string"

    def to_string(self):
        try:
            if self._is_short_string():
                data = self._get_short_string_bytes()
            else:
                data = self._get_long_string_bytes()

            if data is None:
                return "<nested substring>"
            return format_string_bytes(data)
        except gdb.MemoryError:
            return "<invalid memory>"
        except Exception as e:
            return f"<error: {e}>"

    def children(self):
        """Yield (name, value) pairs for the internal fields"""
        try:
            for child in super().yield_children(ignored_fields=["m_impl"]):
                yield child

            impl = self._get_impl()
            is_short = self._is_short_string()

            yield "is_short_string", is_short

            if is_short:
                short_string = impl["short_string"]
                yield "short_string", short_string
            else:
                data_ptr = impl["data"]
                yield "data", data_ptr

        except gdb.MemoryError:
            yield "<error>", "invalid memory"
        except Exception as e:
            yield "<error>", str(e)


class AKStringViewPrinter(GenericPrinter):
    """Pretty-printer for AK::StringView"""

    def __init__(self, val):
        super().__init__(val)

    def get_bytes(self):
        """Extract the string bytes from StringView"""
        characters = self.val["m_characters"]
        length = int(self.val["m_length"])

        if int(characters) == 0 or length == 0:
            return b""

        inferior = gdb.selected_inferior()
        memory = inferior.read_memory(characters, length)
        return bytes(memory)

    def display_hint(self):
        return "string"

    def to_string(self):
        try:
            data = self.get_bytes()
            return format_string_bytes(data)
        except gdb.MemoryError:
            return "<invalid memory>"
        except Exception as e:
            return f"<error: {e}>"


class AKFlyStringPrinter(GenericPrinter):
    """Pretty-printer for AK::FlyString"""

    def __init__(self, val):
        super().__init__(val)

    def to_string(self):
        try:
            # FlyString contains m_data which is a Detail::StringBase
            string_base_printer = AKStringPrinter(self.val["m_data"])
            return string_base_printer.to_string()
        except gdb.MemoryError:
            return "<invalid memory>"
        except Exception as e:
            return f"<error: {e}>"


class AKOptionalPrinter(GenericPrinter):
    """Pretty-printer for AK::Optional"""

    def __init__(self, val):
        super().__init__(val)
        self.has_value = bool(self.val["m_has_value"])
        self.contained_type = self.val.type.strip_typedefs().template_argument(0)

    def to_string(self):
        try:
            if not self.has_value:
                return "Empty"

            value = self.val["m_storage"]
            return value.format_string()
        except gdb.MemoryError:
            return "<invalid memory>"
        except Exception as e:
            return f"<error: {e}>"


def build_pretty_printer():
    pp = gdb.printing.RegexpCollectionPrettyPrinter("AK")
    pp.add_printer("AK::String", "^AK::String$", AKStringPrinter)
    pp.add_printer("AK::Detail::StringBase", "^AK::Detail::StringBase$", AKStringPrinter)
    pp.add_printer("AK::Detail::StringData", "^AK::Detail::StringData$", AKStringDataPrinter)
    pp.add_printer("AK::StringView", "^AK::StringView$", AKStringViewPrinter)
    pp.add_printer("AK::FlyString", "^AK::FlyString$", AKFlyStringPrinter)
    pp.add_printer("AK::Optional", "^AK::Optional<.*>$", AKOptionalPrinter)
    return pp


# Register the pretty-printer
gdb.printing.register_pretty_printer(gdb.current_objfile(), build_pretty_printer())
