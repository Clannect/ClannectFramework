"""Reads ICU's preparsed UCD (ppucd.txt): defaults, then block values, then
per-code-point values, each layer overriding the one before."""

N = 0x110000


def parse_range(text):
    if '..' in text:
        a, b = text.split('..')
        return int(a, 16), int(b, 16)
    v = int(text, 16)
    return v, v


def load(path, wanted):
    """Returns {property: list of N values} for the properties in `wanted`
    (short names; binary properties give True/False), plus the UCD version."""
    values = {p: [None] * N for p in wanted}
    version = None
    defaults = {}
    with open(path, encoding='utf-8') as f:
        for line in f:
            line = line.rstrip('\n')
            if not line or line.startswith('#'):
                continue
            fields = line.split(';')
            kind = fields[0]
            if kind == 'ucd':
                version = fields[1]
                continue
            if kind not in ('defaults', 'block', 'cp', 'unassigned', 'algnamesrange'):
                continue
            lo, hi = parse_range(fields[1])
            props = {}
            for item in fields[2:]:
                if '=' in item:
                    k, v = item.split('=', 1)
                    props[k] = v
                elif item.startswith('-'):
                    props[item[1:]] = False
                elif item:
                    props[item] = True
            if kind == 'defaults':
                defaults = props
            if kind == 'unassigned':
                # Unassigned code points start from the file's defaults, not from their block.
                props = {**defaults, **props}
            for p in wanted:
                if p in props:
                    v = props[p]
                    arr = values[p]
                    for c in range(lo, hi + 1):
                        arr[c] = v
                elif kind == 'unassigned':
                    arr = values[p]
                    for c in range(lo, hi + 1):
                        arr[c] = None
    return values, version
