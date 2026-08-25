# Copyright (c) Meta Platforms, Inc. and affiliates.
"""Minimal execute-surface corpus (MR-04).

Every case here compiles and EXECUTES under the canary mode: the bodies
stay strictly inside the MR-04 machine-code whitelist (locals, basic
arithmetic and comparisons, control flow, iteration over constant
tuples) and avoid the MR-03 integer constant-accumulator policy valve by
carrying loop steps in locals.  This module is the refcount-matrix
minimal tier: interp and jit drift must be identical case by case.
"""


def case_while_sum():
    one = 1
    n = 9
    total = n - n
    i = total
    while i < n:
        total = total + n
        i = i + one
    return total


def case_nested_while():
    one = 1
    outer = 4
    inner = 3
    total = outer - outer
    i = total
    while i < outer:
        j = total - total
        while j < inner:
            total = total + one
            j = j + one
        i = i + one
    return total


def case_compare_and_branch():
    a = 7
    b = 11
    if a < b:
        r = b - a
    elif a == b:
        r = a
    else:
        r = a - b
    if r != 4:
        r = -r
    return r


def case_unary_ops():
    x = 6
    y = -x
    z = ~x
    flag = not y
    if flag:
        return y - z
    return y + z


def case_is_ops():
    a = None
    b = 5
    r = 0
    one = 1
    if a is None:
        r = r + one
    if b is not None:
        r = r + one
    return r


def case_contains_const_tuple():
    needle = 3
    if needle in (1, 2, 3, 5, 8):
        return needle
    return -needle


def case_for_iter_const_tuple():
    total = 0
    for x in (2, 3, 5, 7, 11):
        total = total + x
    return total


def case_floor_and_mod():
    a = 47
    b = 5
    return a // b * b + a % b


def case_early_break():
    one = 1
    limit = 100
    i = limit - limit
    while i < limit:
        if i * i > 50:
            break
        i = i + one
    return i


def case_continue_skip():
    one = 1
    n = 10
    total = n - n
    i = total
    while i < n:
        i = i + one
        if i % 2 == 0:
            continue
        total = total + i
    return total


def case_bool_arith():
    t = True
    f = False
    return t + t + f


def case_zero_division_raises():
    a = 1
    b = a - a
    return a / b


def case_type_error_raises():
    one = 1
    none = None
    return one + none
