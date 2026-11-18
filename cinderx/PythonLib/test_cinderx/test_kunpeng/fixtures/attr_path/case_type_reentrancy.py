"""Observe lifetime and callback counts without relying on a UAF crash."""
import gc
import itertools


def scenario(inherited, method, action, warm):
    events = []
    resurrected = []
    armed = False
    owner = None

    class Handler:
        def __call__(self, name):
            events.append('called')
            return (lambda: 'old') if method else 'old'

        def __del__(self):
            events.append('finalized')
            # Keep memory alive to verify finalization order without a crash.
            resurrected.append(self)

    class Key:
        def __hash__(self):
            return hash('__getattribute__')

        def __eq__(self, other):
            nonlocal armed
            if armed and other == '__getattribute__':
                events.append('eq')
                if action == 'delete':
                    armed = False  # Slot updates can compare again.
                    del owner.__getattr__
                elif action == 'attribute_error':
                    raise AttributeError('comparison callback')
                else:
                    raise ValueError('comparison callback')
            return False

    owner = type('Owner', (), {Key(): None, '__getattr__': Handler()})
    C = type('Child', (owner,), {}) if inherited else owner
    obj = C()

    def read(obj):
        return obj.missing() if method else obj.missing

    for _ in range(200 if warm else 0):
        assert read(obj) == 'old'
    owner.marker = 0
    C.marker = 0
    events.clear()
    armed = True
    value = read(obj)
    expected = ['eq', 'called', 'finalized'] if action == 'delete' else ['eq', 'called']
    assert value == 'old', value
    assert events == expected, (inherited, method, action, warm, events, expected)


was_enabled = gc.isenabled()
gc.disable()
try:
    actions = ('delete', 'attribute_error', 'value_error')
    for args in itertools.product((False, True), (False, True), actions, (False, True)):
        scenario(*args)
finally:
    if was_enabled:
        gc.enable()
