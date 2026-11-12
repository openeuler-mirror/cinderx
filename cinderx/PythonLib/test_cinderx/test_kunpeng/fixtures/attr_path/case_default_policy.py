"""Configuration defaults must not require the attribute-mode opt-in."""
import json
import os
import subprocess
import sys

CODE = '''import json,_cinderx as cx
cx.install_frame_evaluator()
print(json.dumps(dict(flags=cx._attr_path_status(),mode=cx._get_observe_stats()['mode'])))
'''


def run(mode=None, jit='off', **overrides):
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('ATTR_PATH_', 'CINDERX_', 'PYTHONJIT', 'AUTO_JIT', 'PARALLEL_GC_'))}
    env.update(CINDERX_PLUGIN_ENABLE='0', CINDERX_JIT_MODE=jit, CINDERX_JIT_STARTUP_GRACE_MS='0')
    if mode is not None:
        env['ATTR_PATH_MODE'] = mode
    env.update(overrides)
    result = subprocess.run([sys.executable, '-c', CODE], env=env, capture_output=True, text=True, check=True)
    return json.loads(result.stdout)


for mode in (None, 'cinderx_control', 'cinderx'):
    result = run(mode)
    flags = result['flags']
    assert result['mode'] == 'off', result
    assert flags['global_cache'] == flags['inline_call'] == 1, result
    assert flags['split_dict'] == flags['specialization'] == flags['no_exception'] == int(mode == 'cinderx'), result
result = run(jit='observe')
assert result['mode'] == 'observe', result
assert result['flags']['global_cache'] == result['flags']['inline_call'] == result['flags']['split_dict'] == 0, result
result = run(ATTR_PATH_GLOBAL_CACHE='0', ATTR_PATH_INLINE_CALL='0')
assert result['flags']['global_cache'] == result['flags']['inline_call'] == 0, result
result = run('cinderx', ATTR_PATH_SPLIT_HINT='0')
assert result['flags']['split_dict'] == 0, result
result = run(ATTR_PATH_SPLIT_HINT='1')
assert result['flags']['split_dict'] == 1, result
