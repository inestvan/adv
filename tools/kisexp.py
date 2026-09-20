"""Minimal s-expression reader/writer for KiCad files."""
import re, uuid

class Sym(str):
    """Bare symbol token (unquoted)."""
    __slots__ = ()

_tok = re.compile(r'\s*(?:(\()|(\))|"((?:[^"\\]|\\.)*)"|([^\s()"]+))', re.S)

def parse(text):
    pos = 0; stack = [[]]
    n = len(text)
    while pos < n:
        m = _tok.match(text, pos)
        if not m:
            if text[pos:].strip() == '': break
            raise ValueError(f"parse error at {pos}: {text[pos:pos+40]!r}")
        pos = m.end()
        if m.group(1):
            stack.append([])
        elif m.group(2):
            lst = stack.pop(); stack[-1].append(lst)
        elif m.group(3) is not None:
            stack[-1].append(m.group(3).replace('\\"','"').replace('\\\\','\\'))
        elif m.group(4) is not None:
            stack[-1].append(Sym(m.group(4)))
    return stack[0]

def _q(s):
    return '"' + s.replace('\\','\\\\').replace('"','\\"').replace('\n','\\n') + '"'

def dump(node, indent=0):
    """Serialize a node (list) in KiCad-like formatting."""
    if isinstance(node, Sym): return str(node)
    if isinstance(node, str): return _q(node)
    if isinstance(node, (int,float)):
        return fmt_num(node)
    # list
    parts = []
    simple = all(not isinstance(c, list) for c in node)
    head = '(' + ' '.join(dump(c) for c in node if not isinstance(c, list))
    kids = [c for c in node if isinstance(c, list)]
    if not kids:
        return head + ')'
    out = head + '\n'
    for k in kids:
        out += '\t'*(indent+1) + dump(k, indent+1) + '\n'
    out += '\t'*indent + ')'
    return out

def fmt_num(v):
    if isinstance(v, int): return str(v)
    s = f"{v:.6f}".rstrip('0').rstrip('.')
    if s in ('-0',''): s = '0'
    return s

def find(node, key):
    """First child list whose head symbol == key."""
    for c in node:
        if isinstance(c, list) and c and c[0] == key: return c
    return None

def find_all(node, key):
    return [c for c in node if isinstance(c, list) and c and c[0] == key]

def S(*args):
    """Build node: S('at', 1, 2) -> [Sym('at'), 1, 2]; strings stay quoted strings, use Sym for bare."""
    return [Sym(args[0])] + list(args[1:])

def new_uuid():
    return str(uuid.uuid4())

# ---------------- symbol library handling ----------------
_lib_cache = {}
def load_lib(path):
    if path not in _lib_cache:
        _lib_cache[path] = parse(open(path, encoding='utf-8').read())[0]
    return _lib_cache[path]

def get_symbol(lib_root, name):
    for s in find_all(lib_root, 'symbol'):
        if s[1] == name: return s
    raise KeyError(name)

import copy
def flatten_symbol(lib_root, name):
    """Return a deep copy of the symbol with 'extends' resolved (KiCad flattens derived symbols in schematics)."""
    sym = copy.deepcopy(get_symbol(lib_root, name))
    ext = find(sym, 'extends')
    if not ext: return sym
    parent = flatten_symbol(lib_root, ext[1])
    # child props override parent props; parent supplies everything else
    child_props = {p[1]: p for p in find_all(sym, 'property')}
    out = [Sym('symbol'), name]
    for c in parent[2:]:
        if isinstance(c, list) and c[0] == 'property':
            if c[1] in child_props:
                out.append(child_props.pop(c[1]))
            else:
                out.append(c)
        elif isinstance(c, list) and c[0] == 'symbol':
            # rename units parentname_U_S -> name_U_S
            u = copy.deepcopy(c)
            u[1] = name + u[1][len(parent[1]):]
            out.append(u)
        elif isinstance(c, list) and c[0] == 'extends':
            continue
        else:
            out.append(c)
    for p in child_props.values(): out.append(p)
    return out

def symbol_pins(sym):
    """Return list of (number, name, x, y, angle, length, etype) for all units of a (flattened) symbol."""
    pins = []
    def walk(n):
        for c in n:
            if isinstance(c, list):
                if c and c[0] == 'pin':
                    at = find(c, 'at'); ln = find(c, 'length')
                    nm = find(c, 'name'); num = find(c, 'number')
                    pins.append(dict(number=str(num[1]), name=str(nm[1]), x=float(at[1]), y=float(at[2]),
                                     angle=float(at[3]) if len(at) > 3 else 0.0, length=float(ln[1]) if ln else 2.54,
                                     etype=str(c[1])))
                else:
                    walk(c)
    walk(sym)
    return pins
