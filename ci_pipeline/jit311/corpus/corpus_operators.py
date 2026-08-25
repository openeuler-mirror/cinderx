"""Operator x operand-type matrix: BINARY_OP / COMPARE_OP / UNARY_* /
BINARY_SUBSCR / STORE_SUBSCR and the reflected/NotImplemented protocol.

The hottest JIT compilation surface (arithmetic unboxing lives here) and a
rich source of exact TypeError messages. Call sites are exec-generated so
every operator/operand combination compiles to its own bytecode.
"""

# --- operand definitions (names referenced by generated sources) ---


class RAddOnly:
    def __radd__(self, other):
        return ("radd", other)

    def __repr__(self):
        return "<RAddOnly>"


class AddReturnsNI:
    def __add__(self, other):
        return NotImplemented

    def __repr__(self):
        return "<AddReturnsNI>"


class LenOnly:
    def __len__(self):
        return 3


OPERANDS = {
    "int3": "3",
    "int0": "0",
    "negint": "(-7)",
    "bigpos": "(2**62 + 1)",      # 紧邻 compact/tagged 边界
    "big128": "(2**128 + 5)",
    "float25": "2.5",
    "floatneg0": "(-0.0)",
    "floatinf": "float('inf')",
    "floatnan": "float('nan')",
    "true": "True",
    "strab": "'ab'",
    "bytes2": "b'xy'",
    "list12": "[1, 2]",
    "tup12": "(1, 2)",
    "none": "None",
    "raddonly": "RADD_ONLY",
    "addni": "ADD_NI",
}

BINARY_OPS = {
    "add": "+", "sub": "-", "mul": "*", "truediv": "/", "floordiv": "//",
    "mod": "%", "pow": "**", "lshift": "<<", "rshift": ">>",
    "band": "&", "bor": "|", "bxor": "^",
    "lt": "<", "le": "<=", "gt": ">", "ge": ">=", "eq": "==", "ne": "!=",
    "in_": "in", "isnot": "is not",
}

# Curated operand pairs: same-type, mixed, boundary, protocol fallback.
PAIRS = [
    ("int3", "int3"), ("int3", "int0"), ("int3", "float25"),
    ("float25", "int3"), ("float25", "float25"), ("float25", "floatnan"),
    ("bigpos", "int3"), ("bigpos", "bigpos"), ("big128", "float25"),
    ("negint", "bigpos"), ("int3", "true"), ("floatneg0", "float25"),
    ("floatinf", "floatinf"), ("strab", "strab"), ("strab", "int3"),
    ("list12", "list12"), ("list12", "int3"), ("tup12", "tup12"),
    ("bytes2", "bytes2"), ("none", "int3"), ("int3", "raddonly"),
    ("addni", "int3"), ("strab", "list12"),
]

UNARY_OPS = {"neg": "-", "pos": "+", "inv": "~", "not_": "not "}
UNARY_OPERANDS = ["int3", "negint", "bigpos", "float25", "floatnan",
                  "true", "strab", "none", "list12"]

AUG_OPS = {"iadd": "+=", "imul": "*=", "isub": "-=", "ipow": "**="}
AUG_PAIRS = [("int3", "int3"), ("float25", "int3"), ("strab", "strab"),
             ("list12", "list12"), ("bigpos", "bigpos"), ("none", "int3")]

# pow/lshift 遇到巨大右操作数会产生天文位宽的大整数（如 (2**62)**(2**62)），
# 解释器本身就算不完——这类组合排除，代之以既有的小指数对覆盖。
_EXPENSIVE = {("pow", "bigpos"), ("pow", "big128"),
              ("lshift", "bigpos"), ("lshift", "big128"),
              ("ipow", "bigpos"), ("ipow", "big128")}

_SRC = []
for _pname, _sym in sorted(BINARY_OPS.items()):
    for _a, _b in PAIRS:
        if (_pname, _b) in _EXPENSIVE:
            continue
        _SRC.append(
            "def op_{p}__{a}__{b}():\n"
            "    x = {ea}\n    y = {eb}\n    return x {sym} y\n".format(
                p=_pname, a=_a, b=_b,
                ea=OPERANDS[_a], eb=OPERANDS[_b], sym=_sym))
for _pname, _sym in sorted(UNARY_OPS.items()):
    for _a in UNARY_OPERANDS:
        _SRC.append(
            "def op_{p}__{a}():\n"
            "    x = {ea}\n    return {sym}x\n".format(
                p=_pname, a=_a, ea=OPERANDS[_a], sym=_sym))
for _pname, _sym in sorted(AUG_OPS.items()):
    for _a, _b in AUG_PAIRS:
        if (_pname, _b) in _EXPENSIVE:
            continue
        _SRC.append(
            "def op_{p}__{a}__{b}():\n"
            "    x = {ea}\n    x {sym} {eb}\n    return x\n".format(
                p=_pname, a=_a, b=_b,
                ea=OPERANDS[_a], eb=OPERANDS[_b], sym=_sym))

# Subscript family: load / store / delete / slice / protocol errors.
_SRC.append('''
def sub_list_index():
    x = [10, 20, 30]
    return x[1]

def sub_list_neg():
    x = [10, 20, 30]
    return x[-1]

def sub_list_oob():
    x = [10, 20, 30]
    return x[5]

def sub_list_slice():
    x = [0, 1, 2, 3, 4, 5]
    return (x[1:4], x[::2], x[::-1], x[10:20])

def sub_dict_hit():
    d = {"k": 1, 2: "two"}
    return (d["k"], d[2])

def sub_dict_miss():
    d = {"k": 1}
    return d["missing"]

def sub_str_index():
    s = "hello"
    return (s[0], s[-1], s[1:3])

def sub_store_list():
    x = [1, 2, 3]
    x[0] = 99
    x[-1] = 77
    return x

def sub_store_slice():
    x = [1, 2, 3, 4]
    x[1:3] = [9]
    return x

def sub_del():
    x = [1, 2, 3]
    del x[0]
    d = {"a": 1, "b": 2}
    del d["a"]
    return (x, d)

def sub_nonsub():
    x = 5
    return x[0]

def sub_badkey_unhashable():
    d = {}
    return d[[1, 2]]

def sub_index_protocol():
    class Idx:
        def __index__(self):
            return 1
    x = [10, 20, 30]
    return x[Idx()]

def sub_len_only_truth():
    return bool(LEN_ONLY)
''')

_ns = {
    "RADD_ONLY": RAddOnly(),
    "ADD_NI": AddReturnsNI(),
    "LEN_ONLY": LenOnly(),
    "float": float, "bool": bool,
}
exec(compile("\n".join(_SRC), "<diffgate-operators>", "exec"), _ns)

for _name in sorted(_ns):
    if _name.startswith(("op_", "sub_")):
        _fn = _ns[_name]

        def _make_case(fn):
            def case():
                return fn()
            case.helpers = [fn]
            return case

        _case = _make_case(_fn)
        _case.__name__ = "case_" + _name
        globals()[_case.__name__] = _case
del _case, _fn, _name

