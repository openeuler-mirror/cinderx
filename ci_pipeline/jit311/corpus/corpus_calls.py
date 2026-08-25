"""CALL / PRECALL / KW_NAMES / CALL_FUNCTION_EX shape matrix.

Cartesian product: callee kinds x syntactically distinct call sites. Call-site
functions are generated via exec so each shape compiles to its own bytecode
sequence (a single generic dispatcher would collapse everything into one
CALL_FUNCTION_EX site). Results and exact TypeError messages are compared
against the interpreter oracle.
"""

import functools

# --- callee kinds (module level, stable names for error messages) ---


def f_pos2(a, b):
    return ("f_pos2", a, b)


def f_defaults(a, b=10):
    return ("f_defaults", a, b)


def f_kwonly(a, *, k=5):
    return ("f_kwonly", a, k)


def f_star(*args):
    return ("f_star", args)


def f_kwargs(**kw):
    return ("f_kwargs", sorted(kw.items()))


def f_full(a, b=1, *args, k=2, **kw):
    return ("f_full", a, b, args, k, sorted(kw.items()))


class Callee:
    def __init__(self, tag="ctor"):
        self.tag = tag

    def method(self, a, b):
        return ("method", self.tag, a, b)

    @classmethod
    def cmethod(cls, a):
        return ("cmethod", cls.__name__, a)

    @staticmethod
    def smethod(a, b):
        return ("smethod", a, b)

    def __call__(self, a, b):
        return ("dunder_call", self.tag, a, b)

    def __repr__(self):
        return "<Callee {}>".format(self.tag)


_instance = Callee("inst")

CALLEES = {
    "f_pos2": f_pos2,
    "f_defaults": f_defaults,
    "f_kwonly": f_kwonly,
    "f_star": f_star,
    "f_kwargs": f_kwargs,
    "f_full": f_full,
    "bound_method": _instance.method,
    "classmethod": Callee.cmethod,
    "staticmethod": Callee.smethod,
    "callable_obj": _instance,
    "lambda2": (lambda a, b: ("lambda2", a, b)),
    "partial1": functools.partial(f_pos2, 1),
    "builtin_len": len,
    "builtin_max": max,
    "str_join": ",".join,
}

# --- call-site shapes: name -> source of the call expression ---

SHAPES = {
    "noargs": "c()",
    "pos1": "c(1)",
    "pos2": "c(1, 2)",
    "pos3": "c(1, 2, 3)",
    "kw_only_a": "c(a=1)",
    "pos_kw": "c(1, b=2)",
    "kw_swapped": "c(b=2, a=1)",
    "dup_kw": "c(1, a=2)",
    "unexpected_kw": "c(1, zz=9)",
    "star_pair": "c(*(1, 2))",
    "star_then_pos": "c(*(1,), 2)",
    "star_list": "c(*[1, 2])",
    "star_gen": "c(*(x for x in (1, 2)))",
    "dstar_dict": "c(**{'a': 1, 'b': 2})",
    "star_dstar": "c(*(1,), **{'b': 2})",
    "mixed_full": "c(1, *(2,), k=3, **{'zz': 4})",
    "dstar_badkeys": "c(**{1: 'x'})",
    "star_noniter": "c(*None)",
    "kw_seq_args": "c(('a', 'b'))",
}

_SRC = []
for _shape, _expr in sorted(SHAPES.items()):
    _SRC.append("def shape_{}(c):\n    return {}\n".format(_shape, _expr))
_ns = {}
exec(compile("\n".join(_SRC), "<diffgate-shapes>", "exec"), _ns)
SHAPE_FNS = {name: _ns["shape_" + name] for name in SHAPES}


def _make_case(callee_name, shape_name):
    callee = CALLEES[callee_name]
    shape_fn = SHAPE_FNS[shape_name]

    def case():
        return shape_fn(callee)

    helpers = [shape_fn]
    if callable(callee) and hasattr(callee, "__code__"):
        helpers.append(callee)
    case.helpers = helpers
    case.__name__ = "case_{}__{}".format(callee_name, shape_name)
    return case


for _callee_name in sorted(CALLEES):
    for _shape_name in sorted(SHAPES):
        _case = _make_case(_callee_name, _shape_name)
        globals()[_case.__name__] = _case
del _case, _callee_name, _shape_name
