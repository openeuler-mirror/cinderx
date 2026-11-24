import _cinderx as cx
import _hook_probe as probe

cx.install_frame_evaluator()
assert cx._attr_path_status()['inline_call']

def child(x):return x+1
def parent():
    # The current CinderX frame must respect a subsequently installed hook.
    probe.install()
    try:result=child(2)
    finally:count=probe.restore()
    assert result==3 and count==1,(result,count)
for _ in range(100):parent()
assert not any(cx._get_trigger_stats().values())
