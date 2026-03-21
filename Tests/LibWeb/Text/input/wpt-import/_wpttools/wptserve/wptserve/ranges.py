# mypy: allow-untyped-defs

from .utils import HTTPException


class RangeParser:
    def __call__(self, header, file_size):
        try:
            header = header.decode("ascii")
        except UnicodeDecodeError:
            raise HTTPException(400, "Non-ASCII range header value")
        prefix = "bytes="
        if not header.startswith(prefix):
            raise HTTPException(416, message=f"Unrecognised range type {header}")

        parts = header[len(prefix):].split(",")
        ranges = []
        for item in parts:
            components = item.split("-")
            if len(components) != 2:
                raise HTTPException(416, "Bad range specifier %s" % (item))
            data = []
            for component in components:
                if component == "":
                    data.append(None)
                else:
                    try:
                        data.append(int(component))
                    except ValueError:
                        raise HTTPException(416, "Bad range specifier %s" % (item))
            try:
                ranges.append(Range(data[0], data[1], file_size))
            except ValueError:
                raise HTTPException(416, "Bad range specifier %s" % (item))

        return self.coalesce_ranges(ranges, file_size)

    def coalesce_ranges(self, ranges, file_size):
        rv = []
        target = None
        for current in reversed(sorted(ranges)):
            if target is None:
                target = current
            else:
                new = target.coalesce(current)
                target = new[0]
                if len(new) > 1:
                    rv.append(new[1])
        rv.append(target)

        return rv[::-1]


class Range:
    def __init__(self, lower, upper, file_size):
        self.file_size = file_size
        self.lower, self.upper = self._abs(lower, upper)
        if self.lower >= self.upper or self.lower >= self.file_size:
            raise ValueError

    def __repr__(self):
        return f"<Range {self.lower}-{self.upper}>"

    def __lt__(self, other):
        return self.lower < other.lower

    def __gt__(self, other):
        return self.lower > other.lower

    def __eq__(self, other):
        return self.lower == other.lower and self.upper == other.upper

    def _abs(self, lower, upper):
        if lower is None and upper is None:
            lower, upper = 0, self.file_size
        elif lower is None:
            lower, upper = max(0, self.file_size - upper), self.file_size
        elif upper is None:
            lower, upper = lower, self.file_size
        else:
            lower, upper = lower, min(self.file_size, upper + 1)

        return lower, upper

    def coalesce(self, other):
        assert self.file_size == other.file_size

        if (self.upper < other.lower or self.lower > other.upper):
            return sorted([self, other])
        else:
            return [Range(min(self.lower, other.lower),
                          max(self.upper, other.upper) - 1,
                          self.file_size)]

    def header_value(self):
        return "bytes %i-%i/%i" % (self.lower, self.upper - 1, self.file_size)
