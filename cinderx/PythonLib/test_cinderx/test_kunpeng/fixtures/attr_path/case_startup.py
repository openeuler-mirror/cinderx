"""Real .pth subprocess regression: modes, inheritance, fail-closed startup."""
import json,os,subprocess,sys

def clean():
 env=os.environ.copy()
 for k in list(env):
  if k.startswith(('ATTR_PATH_','CINDERX_','PYTHONJIT','AUTO_JIT')):env.pop(k)
 return env
child = """import json, _cinderx_auto
class Subject:
    def __init__(self):
        self.value = 7
    def method(self, delta):
        return self.value + delta
    def __getattr__(self, name):
        return ('fallback', name)

subject = Subject()
access = [subject.value, subject.missing, subject.method(5)]
# Check the installed evaluator and live JIT counters after exercising it.
status = _cinderx_auto._verify()
status['access'] = access
print(json.dumps(status))
"""
parent="""import os,subprocess,sys
assert not any(k.startswith('ATTR_PATH_') and k!='ATTR_PATH_MODE' for k in os.environ),{k:v for k,v in os.environ.items() if k.startswith('ATTR_PATH_')}
env=os.environ.copy();env['ATTR_PATH_MODE']=sys.argv[1]
p=subprocess.run([sys.executable,'-c',sys.argv[2]],env=env,text=True,capture_output=True)
print(p.stdout,end='');print(p.stderr,end='',file=sys.stderr);sys.exit(p.returncode)
"""
access_by_mode = {}
for pm, cm in ((None, 'cinderx'), ('cinderx', 'cinderx_control'),
               ('cinderx_control', 'cinderx'), ('cinderx', 'off')):
 env=clean()
 if pm:env['ATTR_PATH_MODE']=pm
 p=subprocess.run([sys.executable,'-c',parent,cm,child],env=env,text=True,capture_output=True)
 assert p.returncode==0,(pm,cm,p.stderr)
 s=json.loads(p.stdout)
 assert s['access'] == [7, ['fallback', 'missing'], 12], (cm, s)
 access_by_mode[cm] = s['access']
 if cm=='off':assert s['evaluator'] is False
 else:
  assert s['evaluator']
  assert s['attribute']['specialization']==s['attribute']['no_exception']==s['attribute']['split_dict']==int(cm=='cinderx'),s
  assert not any(s['jit'].values())
assert access_by_mode['off'] == access_by_mode['cinderx_control'] == access_by_mode['cinderx']
for override in ('0','1'):
 env=clean();env.update(ATTR_PATH_MODE='cinderx',ATTR_PATH_SPECIALIZE=override)
 p=subprocess.run([sys.executable,'-c',child],env=env,text=True,capture_output=True,check=True)
 assert json.loads(p.stdout)['attribute']['specialization']==int(override)
for key,value in [('ATTR_PATH_GLOBAL_ROUTE','bad'),('ATTR_PATH_INLINE_CALL','bad'),('ATTR_PATH_SPECIALIZE','bad'),('CINDERX_DISABLE','1')]:
 env=clean();env.update(ATTR_PATH_MODE='cinderx');env[key]=value
 p=subprocess.run([sys.executable,'-c','print("APP_STARTED")'],env=env,text=True,capture_output=True)
 assert p.returncode!=0 and 'APP_STARTED' not in p.stdout and 'ATTR_PATH_STARTUP_FAILED' in p.stderr,(key,p.returncode,p.stdout,p.stderr)
# Verify compares actual state with resolved expectations, not just JIT state.
env=clean();env['ATTR_PATH_MODE']='cinderx'
code="""import _cinderx_auto as a,_cinderx
f=_cinderx._attr_path_status
def wrong():
 s=f();s['specialization']=0;return s
_cinderx._attr_path_status=wrong
try:a._verify()
except RuntimeError:print('MISMATCH_CAUGHT')
else:raise AssertionError('configuration mismatch accepted')
"""
p=subprocess.run([sys.executable,'-c',code],env=env,text=True,capture_output=True,check=True)
assert 'MISMATCH_CAUGHT' in p.stdout
