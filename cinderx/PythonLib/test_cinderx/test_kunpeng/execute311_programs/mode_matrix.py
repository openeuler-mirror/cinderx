"""One hot function under whichever mode the parent configured."""
from _harness import T, creations, emit, entries, events, observe, trigger
from _fixtures import cinderjit, hot

before = trigger()
values = [hot(i, 4) for i in range(T + 10)]
after = trigger()
stats = observe()
emit(
    values_ok=values == [i * 4 for i in range(T + 10)],
    enabled=stats["enabled"],
    mode=stats["mode"],
    results=[e["result"] for e in events("hot")],
    entries=after["machine_code_entries"] - before["machine_code_entries"],
    creations=after["compiled_function_creations"],
    shadow=after["shadow_compile_success"],
    allocs=after["executable_alloc_calls"],
    cinderjit=cinderjit is not None,
    compiled=(cinderjit is not None and cinderjit.is_jit_compiled(hot)),
)
