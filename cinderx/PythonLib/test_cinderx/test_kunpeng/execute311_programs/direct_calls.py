# Copyright (c) Meta Platforms, Inc. and affiliates.

"""Exercise every target-selection arm of CPython 3.11 generated calls."""

import functools

from _fixtures import cinderjit
from _harness import emit, entries


class Callable:
    def __call__(self, value):
        return value + 7


class Holder:
    def __init__(self, function):
        self.function = function

    def call(self, value):
        # An instance attribute produces LOAD_METHOD's null-receiver shape;
        # JITRT_Call must keep ownership of its argument-window shift.
        return self.function(value)


class Box:
    def add(self, value):
        return value + 3


def python_function(value):
    return value + 1


def dispatch(function, value):
    return function(value)


def dispatch_kw(function, value, multiplier):
    return function(value, multiplier=multiplier)


def dispatch_method(box, value):
    return box.add(value)


def keyword_target(value, *, multiplier=2):
    return value * multiplier


def raises(value):
    raise ValueError(value)


for function in (dispatch, dispatch_kw, dispatch_method, Holder.call):
    assert cinderjit.force_compile(function) is True, function.__qualname__

before = entries()
generic_results = [
    dispatch(python_function, 5),
    dispatch(len, [1, 2, 3]),
    dispatch(Callable(), 5),
    dispatch("abc".index, "b"),
    dispatch(functools.partial(python_function), 5),
    dispatch(int, "42"),
]
keyword_results = [
    dispatch_kw(keyword_target, 3, 4),
    dispatch_kw(keyword_target, 5, 3),
]
method_results = [dispatch_method(Box(), 4), Holder(python_function).call(8)]
try:
    dispatch(raises, 9)
except ValueError as error:
    exception = error.args
else:
    raise AssertionError("the direct Python-function arm swallowed an exception")

emit(
    compiled={
        function.__qualname__: cinderjit.is_jit_compiled(function)
        for function in (dispatch, dispatch_kw, dispatch_method, Holder.call)
    },
    generic_results=generic_results,
    keyword_results=keyword_results,
    method_results=method_results,
    exception=exception,
    entry_delta=entries() - before,
)
