"""Positive qualification cache: invalidation, raw dict writes and lifetime."""
import gc,weakref
import _cinderx as cx
import _key_probe as probe
def stats(): return cx._attr_mro_stats()
def read(o):return o.x
def warm(o):
 for _ in range(100):read(o)
class Base:
 def __getattr__(self,n):return 'old'
class C(Base):pass
o=C();warm(o)
b=stats();warm(o);a=stats()
assert a['cache_enabled'] and a['hits'] > b['hits']
C.marker=1;b=stats();assert read(o)=='old';warm(o)
Base.__getattr__=lambda self,n:'new';assert read(o)=='new';warm(o)
C.__getattribute__=lambda self,n:77 if n=='x' else object.__getattribute__(self,n)
assert read(o)==77
b=stats();warm(o);a=stats()
assert a['cached_fallbacks'] > b['cached_fallbacks']
del C.__getattribute__;assert read(o)=='new';warm(o)
events=[];armed=False
class Key:
 def __hash__(self):return hash('__getattribute__')
 def __eq__(self,other):
  if armed and other=='__getattribute__':events.append('eq');raise AttributeError('compare')
  return False
Unsafe=type('Unsafe',(),{Key():None,'__getattr__':lambda self,n:'unsafe'})
C.__bases__=(Unsafe,);events.clear();armed=True
assert read(o)=='unsafe' and events==['eq'],events
armed=False;C.__bases__=(Base,);assert read(o)=='new';warm(o)
# Direct dictionary mutation, then clear only the host lookup cache. The
# type version remains valid: the host-cache shortcut must miss and run the original MRO guard.
namespace=gc.get_referents(C.__dict__)[0];assert isinstance(namespace,dict)
v=probe.type_info(C);key=Key();namespace[key]=None
probe.clear_type_cache();assert probe.type_info(C)==v
armed=True;events.clear();assert read(o)=='new' and events==['eq'],events
armed=False;del namespace[key]
D=type('D',(Base,),{});d=D();warm(d)
base_namespace=gc.get_referents(Base.__dict__)[0]
v=probe.type_info(D);base_key=Key();base_namespace[base_key]=None
probe.clear_type_cache();assert probe.type_info(D)==v
armed=True;events.clear();assert read(d)=='new' and events==['eq'],events
armed=False;del base_namespace[base_key]
def temporary():
 T=type('Temp',(),{'__getattr__':lambda self,n:1});x=T();warm(x);return weakref.ref(T)
refs=[temporary() for _ in range(300)];gc.collect();assert all(ref() is None for ref in refs)
class Oracle:
 def __getattr__(self,n):return 'oracle'
q=Oracle();raw=gc.get_referents(Oracle.__dict__)[0];raw[Key()]=None
observed=[]
for getter in (read,lambda x:getattr(x,'x')):
 armed=False
 for _ in range(100):getattr(q,'x')
 events.clear();armed=True
 value=getter(q);observed.append((value,events[:]))
armed=False
assert observed[0]==observed[1],observed
cx.remove_frame_evaluator();cx.install_frame_evaluator();assert read(o)=='new'
assert not any(cx._get_trigger_stats().values())
