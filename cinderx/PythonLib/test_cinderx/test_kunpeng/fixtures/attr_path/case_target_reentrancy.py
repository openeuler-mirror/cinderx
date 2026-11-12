"""Warm qualification names must not admit a reentrant cold target lookup.

Run in fresh processes under scan-only and cache-probe modes.
"""
import gc
import sys

import _cinderx as cx
import _key_probe as probe


def run_case(form, exception, warmed_target, class_present=True):
    events = []
    armed = False
    target = sys.intern('target_reentrant_attribute')
    qualification = [sys.intern('__getattribute__'), sys.intern('__getattr__')]
    class_value = (lambda: 'class-value') if form == 'method' else 'class-value'

    class InstanceKey:
        def __hash__(self):
            return hash(target)

        def __eq__(self, other):
            if other == target:
                events.append('instance-eq')
                raise exception('instance compare')
            return False

    class TypeKey:
        def __hash__(self):
            return hash(target)

        def __eq__(self, other):
            if armed and other == target:
                events.append('type-eq')
                object.__setattr__(obj, '__dict__', {InstanceKey(): None})
            return False

    def fallback(self, name):
        events.append('fallback:' + name)
        return (lambda: 'fallback:x') if form == 'method' else 'fallback:x'

    base_values = {target: staticmethod(class_value) if form == 'method' else class_value} if class_present else {}
    base = type('TargetBase', (), base_values)
    cls = type('TargetChild', (base,), {TypeKey(): None, '__getattr__': fallback})
    obj = cls()
    obj.__dict__ = {}
    namespace = {}
    exec('def read(obj):\n    return obj.' + target + ('()' if form == 'method' else ''), namespace)
    read = namespace['read']
    # Warm only qualification names via an unrelated missing name. In the
    # warm-target control the target lookup is cached without arming callbacks.
    for _ in range(20):
        getattr(obj, 'unrelated_missing_name')
        if warmed_target:
            getattr(obj, target)
    assert all(probe.type_cache_info(cls, n)['hit'] for n in qualification)
    cache_target = probe.type_cache_info(cls, target)
    assert bool(cache_target['hit']) == warmed_target, cache_target
    if warmed_target:
        assert bool(cache_target['has_value']) == class_present, cache_target
    events.clear()
    before = cx._attr_mro_stats()
    armed = True
    try:
        value = read(obj)
        outcome = ['value', value]
    except Exception as error:
        outcome = [type(error).__name__, str(error)]
    armed = False
    after = cx._attr_mro_stats()
    delta = {k: after[k] - before[k] for k in before if k not in ('cache_enabled', 'counting')}
    if warmed_target:
        expected = ['value', 'class-value' if class_present else 'fallback:x']
        expected_events = [] if class_present else ['fallback:' + target]
    elif exception is AttributeError:
        expected = ['value', 'fallback:x']
        expected_events = ['type-eq', 'instance-eq', 'fallback:' + target]
    else:
        expected = ['ValueError', 'instance compare']
        expected_events = ['type-eq', 'instance-eq']

    row = dict(form=form, exception=exception.__name__, target_warm=warmed_target, class_present=class_present,
               outcome=outcome, events=events, target_cache=cache_target, delta=delta)
    assert outcome == expected and events == expected_events, row
    if before['cache_enabled']:
        if warmed_target:
            assert delta['calls'] == delta['hits'] == 1, row
        else:
            # A cold target must force the reviewed MRO guard before callbacks.
            assert delta['calls'] == delta['scans'] == delta['rejects'] == 1, row
            assert delta['hits'] == 0, row


enabled = gc.isenabled()
gc.disable()
try:
    for form in ('attribute', 'method'):
        for error in (AttributeError, ValueError):
            run_case(form, error, warmed_target=False)
        for class_present in (False, True):
            run_case(form, AttributeError, warmed_target=True, class_present=class_present)
finally:
    if enabled:
        gc.enable()
assert not any(cx._get_trigger_stats().values())
