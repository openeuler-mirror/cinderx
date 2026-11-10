import cinderjit
import cinderx.jit as jit

SRC = "def probe(a, b):\n    total = a + b\n    return total * 2 + a\n"


def make_twin():
    ns = {}
    exec(compile(SRC, "<dedup_probe>", "exec"), ns)
    return ns["probe"]


def warm(func):
    # Wait for the bounded IC gate without bypassing AutoJIT or donor promotion.
    for value in range(512):
        assert func(value, value) == 5 * value
        if jit.is_jit_compiled(func):
            return True
    return jit.is_jit_compiled(func)


twin1 = make_twin()
assert warm(twin1), "first twin should compile under the released budget"

twin2 = make_twin()
assert warm(twin2), "second twin compiles and promotes the donor"

twin3 = make_twin()
assert warm(twin3), "third twin should attach to the donor"
donor_code = twin3.__code__
assert twin2.__code__ is donor_code, "twin3 should be canonicalized onto the donor"

# Gut the donor artifact the way force_uncompile / ROI backoff does; the
# dedup entry must be demoted instead of keeping the cleared artifact.
assert cinderjit.force_uncompile(twin2)

twin4 = make_twin()
assert warm(twin4), "twin after uncompile must recompile cleanly"
assert twin4.__code__ is not donor_code, (
    "twin4 must not be attached to the gutted donor artifact"
)

twin5 = make_twin()
assert warm(twin5), "recurrence after re-promotion attaches again"
assert twin5.__code__ is twin4.__code__, "twin5 should canonicalize onto the re-promoted donor"
