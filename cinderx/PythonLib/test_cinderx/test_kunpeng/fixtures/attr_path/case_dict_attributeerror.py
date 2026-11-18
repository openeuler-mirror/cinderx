"""A dictionary equality AttributeError must dispatch to the original handler."""
for managed in (True,False):
 for method in (False,True):
  events=[]
  class Collision:
   def __hash__(self):return hash('x')
   def __eq__(self,other):events.append('eq');raise AttributeError('comparison failed')
  class Descriptor:
   def __get__(self,obj,tp):events.append('descriptor');return lambda:'class-method'
  ns={'__getattr__':lambda self,name:(lambda:'fallback:'+name) if method else 'fallback:'+name,'x':Descriptor() if method else 'class-value'}
  C=type('C',(object,) if managed else (Exception,),ns)
  # Py_TPFLAGS_MANAGED_DICT is (1 << 4) in the pinned CPython 3.11.6 headers.
  assert bool(C.__flags__ & (1 << 4)) == managed, (managed,C.__flags__)
  assert C.__dictoffset__ != 0
  obj=C();obj.__dict__={Collision():0}
  def read(obj):return obj.x() if method else obj.x
  result=read(obj);assert result=='fallback:x',result;assert events==['eq'],events
