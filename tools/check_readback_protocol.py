from pathlib import Path
s=(Path(__file__).resolve().parents[1]/'runtime/src/ps2_video.c').read_text()
present=s[s.index('static void present_frame('):s.index('static void gpu_speed_collect(')]
assert 'read_buf, 1,' not in present
assert 'RT_MAX * RT_MAX * 4 * RT_SLOTS' not in s
assert s.count('readback_targets(')==3
helper=s[s.index('static int readback_targets('):s.index('void ps2_vk_statecap(')]
assert 'vkQueueWaitIdle(queue)' in helper
assert 'VK_ACCESS_HOST_READ_BIT' in helper
assert 'VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, previous' in helper
assert 'if (need > read_capacity)' in helper
renderer=[('lock','gpu'),('work','draw'),('unlock','gpu'),('lock','list'),('work','done'),('unlock','list')]
capture=[('lock','list'),('lock','gpu'),('work','copy_and_write'),('unlock','gpu'),('unlock','list')]
seen=set(); completed=0
def visit(pc,owners):
 global completed
 key=(tuple(pc),tuple(owners))
 if key in seen:return
 seen.add(key)
 if pc==[len(renderer),len(capture)]: completed+=1;return
 moves=0
 for thread,program in enumerate([renderer,capture]):
  if pc[thread]==len(program):continue
  op,item=program[pc[thread]]; newowners=owners[:]
  if op!='work':
   lock=['gpu','list'].index(item)
   if op=='lock':
    if owners[lock]!=-1:continue
    newowners[lock]=thread
   else:
    assert owners[lock]==thread
    newowners[lock]=-1
  elif item=='copy_and_write':assert owners==[thread,thread]
  else:assert owners[0 if item=='draw' else 1]==thread
  moves+=1;newpc=pc[:];newpc[thread]+=1;visit(newpc,newowners)
 assert moves,('deadlock',pc,owners)
visit([0,0],[-1,-1])
assert completed
print(f'PASS: {len(seen)} lock states; no lock-order deadlock; normal color readback absent; allocation and host visibility source checks passed')
