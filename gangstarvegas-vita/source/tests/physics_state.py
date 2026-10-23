"""Exercise linked Vita observer ABI and original C++ body-base adjustment.

Native Havok/streamer getters are query boundaries; solver/collision behavior
requires hardware comparison. Game objects and native code are not written.
"""
from player_state import machine,PLAYER,NODE,BASE,store,PROFILE,PHYSICS
WORLD,STREAMER,HAVOK,FIXED,BODY,SHAPE=0x104000,0x105000,0x106000,0x107000,0x108000,0x109000
RAW,VTABLE=0x101000,0x101800


def main():
    cpu,symbols,singleton,state,records,calls,call=machine()
    cpu.mem_map(WORLD,32768)
    assert call('vegas_bind_player_probe')==0
    store(cpu,singleton,PLAYER);store(cpu,PLAYER,BASE+PROFILE['vptr'])
    store(cpu,PLAYER+PROFILE['node'],NODE)
    store(cpu,PLAYER+PROFILE['physics_reference'],RAW)
    store(cpu,RAW,VTABLE);store(cpu,VTABLE-12,0x74)
    store(cpu,BASE+PHYSICS['world_slot'],WORLD)
    store(cpu,WORLD+PHYSICS['world_streamer'],STREAMER)
    store(cpu,WORLD+PHYSICS['world_havok'],HAVOK)
    store(cpu,WORLD+PHYSICS['world_body_begin'],SHAPE)
    store(cpu,WORLD+PHYSICS['world_body_end'],SHAPE+188*4)
    store(cpu,STREAMER+PHYSICS['streamer_grid'],SHAPE)
    store(cpu,HAVOK+PHYSICS['fixed_island'],FIXED)
    store(cpu,FIXED+PHYSICS['island_entity_count'],181)
    store(cpu,BODY+PHYSICS['body_world'],HAVOK)
    store(cpu,BODY+PHYSICS['body_shape'],SHAPE)
    cpu.mem_write(BODY+PHYSICS['body_motion_type'],bytes([7]));state['body']=BODY
    before=bytes(cpu.mem_read(PLAYER,49152))
    call('vegas_trace_player_state',300)
    sample=state['physics_logs'][-1]
    assert (sample['world'],sample['havok'],sample['streamer'],sample['grid'])==(WORLD,HAVOK,STREAMER,SHAPE)
    assert (sample['bodies'],sample['fixed'],sample['done'],sample['district'],sample['anchor_district'])==(188,181,1,1,1)
    assert (sample['physics'],sample['body'],sample['body_world'],sample['shape'],sample['motion'],sample['member'])==(RAW+0x74,BODY,HAVOK,SHAPE,7,1)
    assert ('body',WORLD,RAW+0x74) in state['query_calls']
    assert bytes(cpu.mem_read(PLAYER,49152))==before
    state['xyz']=(10,20,-70)
    call('vegas_trace_player_state',600)
    sample=state['physics_logs'][-1]
    assert sample['district']==0 and sample['anchor_district']==1
    store(cpu,BODY+PHYSICS['body_world'],0)
    call('vegas_trace_player_state',900)
    assert state['physics_logs'][-1]['member']==0 and state['physics_logs'][-1]['vehicle']==0
    state['vehicle']=1
    call('vegas_trace_player_state',901)
    call('vegas_trace_player_state',1200)
    assert state['physics_logs'][-1]['member']==0 and state['physics_logs'][-1]['vehicle']==1
    state['body']=0;store(cpu,STREAMER+PHYSICS['streamer_grid'],0)
    state['query_calls'].clear()
    call('vegas_trace_player_state',1200)
    sample=state['physics_logs'][-1]
    assert sample['done']==sample['district']==sample['anchor_district']==0xffffffff
    assert sample['body']==sample['shape']==sample['member']==0
    assert not any(c[0] in ('done','district') for c in state['query_calls'])
    store(cpu,PLAYER+PROFILE['physics_reference'],0);state['query_calls'].clear()
    call('vegas_trace_player_state',1500)
    assert not state['query_calls'] and state['physics_logs'][-1]['physics']==0
    store(cpu,BASE+PHYSICS['world_slot'],0)
    call('vegas_trace_player_state',1800)
    assert 'world=absent' in state['physics_logs'][-1]['format']
    for name in ('get_physics_body','get_hkp_body','streamer_done','streamer_district_loaded'):
        for delta in (0,4):
            cpu,symbols,singleton,state,records,calls,call=machine()
            store(cpu,BASE+PHYSICS['functions'][name][0]+delta,0)
            assert call('vegas_bind_player_probe')==0xffffffff
            assert 'physics probe profile mismatch' in records[-1]['format']
    for delta in (0,4):
        cpu,symbols,singleton,state,records,calls,call=machine()
        store(cpu,BASE+PROFILE['in_vehicle']+delta,0)
        assert call('vegas_bind_player_probe')==0xffffffff
    print('PASS: actual linked Vita physics observer, original body adjustment, counts/membership/shape, current vs anchor, null body/world/grid, read-only objects and vehicle state and all ten guard failures')


if __name__=='__main__':main()
