"""Framework-free differential cases; run in a fresh process per evaluator."""
import gc,json,os,sys,threading,types,weakref
from pathlib import Path

cx = None
if os.environ['ATTR_PATH_MODE'] == 'cinderx':
    import _cinderx as cx
    assert cx.is_frame_evaluator_installed()
    assert cx._attr_path_status()['no_exception']

def run_cases():
    result={};events=[]
    def capture(key,fn,obj=None):
        try:result[key]=dict(value=fn())
        except BaseException as e:
            result[key]=dict(error=type(e).__name__,message=str(e),name=getattr(e,'name',None),
                obj_matches=getattr(e,'obj',None) is obj,context=type(e.__context__).__name__ if e.__context__ else None)
    class Basic:
        def __init__(self):self.present=23
        def __getattr__(self,name):events.append(('basic',name));return ('fallback',name)
    b=Basic()
    capture('instance_hit',lambda:b.present)
    capture('instance_miss',lambda:b.missing)
    capture('builtin_getattr',lambda:getattr(b,'missing'))
    capture('builtin_hasattr',lambda:hasattr(b,'missing'))
    b.__getattr__=lambda n:('wrong-instance-hook',n)
    capture('instance_hook_ignored',lambda:b.another)
    class Missing:
        def __getattr__(self,name):raise AttributeError('final '+name)
    m=Missing();capture('final_error',lambda:m.absent,m)
    capture('builtin_default',lambda:getattr(m,'absent',99),m)
    class ExistingContext:
        def __getattr__(self,name):raise AttributeError('kept',name='original',obj='sentinel')
    e=ExistingContext();capture('existing_error_context',lambda:e.absent,e)
    class ErrorSubclass(AttributeError):pass
    class CustomError:
        def __getattr__(self,name):raise ErrorSubclass('subclass')
    e=CustomError();capture('error_subclass',lambda:e.absent,e)
    class NoCallable:__getattr__=None
    e=NoCallable();capture('noncallable_hook',lambda:e.absent,e)
    class Descriptor:
        def __get__(self,obj,owner):events.append(('descriptor',owner.__name__));return 37
        def __set__(self,obj,value):raise RuntimeError('read only')
    class DescriptorOwner(Basic):value=Descriptor()
    e=DescriptorOwner();e.__dict__['value']=88
    capture('data_descriptor_priority',lambda:e.value,e)
    class Props(Basic):
        @property
        def good(self):events.append('good');return 41
        @property
        def absent(self):events.append('absent');raise AttributeError('property miss')
        @property
        def bad(self):events.append('bad');raise ValueError('property error')
    e=Props();capture('property_hit',lambda:e.good,e);capture('property_attributeerror',lambda:e.absent,e);capture('property_other_error',lambda:e.bad,e)
    class Slots:
        __slots__=('field',)
        def __getattr__(self,name):return ('slot fallback',name)
    e=Slots();capture('uninitialized_slot',lambda:e.field,e);e.field=17;capture('initialized_slot',lambda:e.field,e)
    class CustomAccess(Basic):
        def __getattribute__(self,name):
            events.append(('custom-access',name))
            return object.__getattribute__(self,name)
    e=CustomAccess();capture('custom_access_hit',lambda:e.present,e);capture('custom_access_miss',lambda:e.missing,e)
    class Toggle(Basic):pass
    e=Toggle();capture('before_access_override',lambda:e.missing,e)
    Toggle.__getattribute__=lambda self,name:('override',name)
    capture('after_access_override',lambda:e.missing,e)
    del Toggle.__getattribute__
    capture('after_access_restore',lambda:e.missing,e)
    class Changing:
        def __getattr__(self,name):return ('old',name)
        @property
        def missing(self):
            Changing.__getattr__=lambda self,name:('new',name)
            raise AttributeError('change hook')
    e=Changing();capture('handler_snapshot',lambda:e.missing,e);capture('handler_replacement',lambda:e.next,e)
    class Deleted:
        def __getattr__(self,name):return ('retained',name)
        @property
        def missing(self):del Deleted.__getattr__;raise AttributeError('delete hook')
    e=Deleted();capture('handler_deletion_snapshot',lambda:e.missing,e);capture('after_handler_deletion',lambda:e.next,e)
    class A:
        def __getattr__(self,name):return ('A',name)
    class B:
        def __getattr__(self,name):return ('B',name)
    class Child(A):pass
    e=Child();capture('before_base_change',lambda:e.missing,e);Child.__bases__=(B,);capture('after_base_change',lambda:e.missing,e)
    class Old:
        def __getattr__(self,name):return ('old class hook',type(self).__name__,name)
        @property
        def missing(self):self.__class__=New;raise AttributeError('class changed')
    class New:
        def __getattr__(self,name):return ('new class hook',name)
    e=Old();capture('class_change_snapshot',lambda:e.missing,e);capture('after_class_change',lambda:e.next,e)
    class HandlerDescriptor:
        def __get__(self,obj,owner):events.append(('handler-bind',owner.__name__));return lambda name:('bound-hook',name)
    class Exotic:__getattr__=HandlerDescriptor()
    e=Exotic();capture('handler_descriptor',lambda:e.missing,e)
    class Static:
        @staticmethod
        def __getattr__(name):return ('static',name)
    e=Static();capture('static_handler',lambda:e.missing,e)
    class ClassHook:
        @classmethod
        def __getattr__(cls,name):return (cls.__name__,name)
    e=ClassHook();capture('classmethod_handler',lambda:e.missing,e)
    class Meta(type):
        def __getattr__(cls,name):return ('metaclass',name)
    class WithMeta(metaclass=Meta):pass
    capture('metaclass_fallback',lambda:WithMeta.missing,WithMeta)
    mod=types.ModuleType('example');mod.__getattr__=lambda name:('module',name)
    capture('module_fallback',lambda:mod.missing,mod)
    class Methods(Basic):
        def real(self,x):return x+1
        def __getattr__(self,name):return lambda x:('dynamic-method',name,x)
    e=Methods();capture('load_method_hit',lambda:e.real(4),e);capture('load_method_miss',lambda:e.dynamic(4),e)
    capture('active_exception_context',lambda:outer_exception(b))
    class Recursive:
        def __getattr__(self,name):return self.again
    try:Recursive().missing
    except RecursionError:result['recursive_missing']='RecursionError'
    else:raise AssertionError('recursive lookup must fail')
    ready=threading.Event();done=threading.Event()
    class Threaded:
        def __getattr__(self,name):return ('thread-old',name)
        @property
        def missing(self):ready.set();assert done.wait(5);raise AttributeError('thread mutation')
    def change():
        assert ready.wait(5)
        Threaded.__getattr__=lambda self,name:('thread-new',name)
        done.set()
    t=threading.Thread(target=change);t.start();e=Threaded();capture('thread_handler_snapshot',lambda:e.missing,e);t.join()
    capture('thread_handler_after',lambda:e.next,e)
    for _ in range(100):
        class Ephemeral:
            def __getattr__(self,name):return name
        assert Ephemeral().missing=='missing'
        ref=weakref.ref(Ephemeral);del Ephemeral;gc.collect();assert ref() is None
    result['type_lifetime']='collected'
    traced=[]
    def trace(frame,event,arg):
        if frame.f_code.co_filename==__file__ and frame.f_code.co_name in ('traced_lookup','__getattr__'):
            traced.append((frame.f_code.co_name,event,type(arg[1]).__name__ if event=='exception' else None))
        return trace
    def traced_lookup(obj):return obj.missing
    e=Basic();sys.settrace(trace)
    try:traced_value=traced_lookup(e)
    finally:sys.settrace(None)
    result['tracing']=dict(value=traced_value,events=traced)
    result['events']=events
    return result

def outer_exception(obj):
    try:raise KeyError('outer')
    except KeyError:
        before=sys.exception();value=obj.from_except
        return (value,sys.exception() is before)

cases = run_cases()
if cx:
    assert not any(cx._get_trigger_stats().values())
    assert cx._attr_path_counters()['attribute_misses'] > 0
Path(os.environ['ATTR_PATH_TEST_OUTPUT']).write_text(json.dumps(cases), encoding='utf-8')
