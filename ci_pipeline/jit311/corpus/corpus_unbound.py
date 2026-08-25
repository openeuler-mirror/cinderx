"""Unbound local / cell / freevar matrix（3.11 无 LOAD_FAST_CHECK 的 SEGV 高危区）.

3.11 has no LOAD_FAST_CHECK: the JIT must prove definite assignment or emit
its own guards. Every case here reaches a maybe-unbound name on some path and
records either the value or the exact exception type+message the interpreter
produces. No expected values are hardcoded — the interp mode is the oracle.
"""


def case_read_before_assign():
    if False:
        x = 1
    return x  # noqa: F821


def case_del_then_read():
    x = 1
    del x
    return x  # noqa: F821


def case_conditional_true_path():
    flag = True
    if flag:
        x = 41
    return x + 1


def case_conditional_false_path():
    flag = False
    if flag:
        x = 41
    return x + 1


def case_augassign_before_bind():
    x += 1  # noqa: F821
    return x


def case_try_raises_before_bind():
    try:
        x = 1 // 0
    except ZeroDivisionError:
        pass
    return x  # noqa: F821


def case_except_var_after_handler():
    try:
        raise ValueError("boom")
    except ValueError as e:
        pass
    return e  # noqa: F821  (3.11 deletes the except target)


def case_except_var_inside_handler():
    try:
        raise ValueError("boom")
    except ValueError as e:
        return str(e)


def case_loop_var_zero_iterations():
    for item in ():
        pass
    return item  # noqa: F821


def case_loop_var_after_iterations():
    for item in (1, 2, 3):
        pass
    return item


def case_while_else_unbound():
    n = 0
    while n > 0:
        y = n
        n -= 1
    else:
        pass
    return y  # noqa: F821


def case_return_expr_maybe_unbound():
    data = []
    for v in data:
        result = v * 2
    return result  # noqa: F821


def case_fstring_unbound():
    if False:
        name = "x"
    return f"hello {name}"  # noqa: F821


def case_finally_unbound():
    try:
        pass
    finally:
        return z  # noqa: F821


def case_subscript_store_unbound():
    x[0] = 1  # noqa: F821
    return x


def case_annotation_only_then_read():
    x: int
    return x  # noqa: F821


def case_match_no_capture():
    value = 42
    match value:
        case str() as captured:
            pass
    return captured  # noqa: F821


def case_star_unpack_partial():
    def gen():
        yield 1
        raise RuntimeError("mid-iteration")

    try:
        a, *b = gen()
    except RuntimeError:
        pass
    return a  # noqa: F821


def case_walrus_dead_branch():
    if False:
        y = (w := 10)
    return w  # noqa: F821


def case_freevar_unbound_closure():
    def inner():
        return outer_x  # noqa: F821

    result = inner()
    outer_x = 1
    return result


def case_freevar_bound_closure():
    outer_x = 7

    def inner():
        return outer_x

    return inner()


def case_cell_del_after_closure():
    outer_x = 7

    def inner():
        return outer_x

    del outer_x
    return inner()


def case_nonlocal_del_then_read():
    outer_x = 7

    def inner():
        nonlocal outer_x
        del outer_x
        return outer_x

    return inner()


def case_nonlocal_write_then_read():
    outer_x = 1

    def inner():
        nonlocal outer_x
        outer_x = 99

    inner()
    return outer_x


def case_class_body_skips_function_scope():
    marker = "function-local"  # noqa: F841

    class K:
        try:
            attr = marker  # noqa: F821
        except NameError as e:
            attr = "NameError: {}".format(e)

    return K.attr


def case_comprehension_reads_outer_unbound():
    if False:
        base = 10
    return [base + i for i in range(3)]  # noqa: F821


def case_genexpr_late_binding():
    values = [1, 2]
    g = (v * factor for v in values)  # noqa: F821
    factor = 10
    return list(g)


def case_genexpr_name_deleted_before_consume():
    factor = 10
    values = [1, 2]
    g = (v * factor2 for v in values)  # noqa: F821
    del factor
    try:
        return list(g)
    except NameError as e:
        return "NameError: {}".format(e)


def case_global_never_set():
    global _diffgate_never_set_global
    return _diffgate_never_set_global  # noqa: F821


def case_generator_body_unbound_first_next():
    def gen():
        if False:
            x = 1
        yield x  # noqa: F821

    return next(gen())


def case_unbound_in_nested_try_finally():
    try:
        try:
            raise KeyError("k")
        finally:
            if False:
                v = 1
            probe = v  # noqa: F821
    except (KeyError, UnboundLocalError, NameError) as e:
        return "{}: {}".format(type(e).__name__, e)
