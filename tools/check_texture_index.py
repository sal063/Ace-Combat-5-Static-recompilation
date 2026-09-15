import random
rng=random.Random(620)
cache=[None]*256
preferred=[None]*1024
members=[0]*1024
next_slot=0
old_checks=new_checks=0
def bucket(key): return ((key*0x9e3779b1) >> 6) % 32
for step in range(50000):
    if step % 2341 == 0:
        cache=[None]*256; members=[0]*1024
    if rng.random()<0.55:
        key=rng.randrange(512); b=bucket(key);slot=next_slot%256;next_slot+=1
        old=cache[slot]
        if old: members[bucket(old[0])] &= ~(1<<slot)
        cache[slot]=(key,rng.randrange(4)); members[b] |= 1<<slot; preferred[b]=slot
    else:
        key=rng.randrange(512); b=bucket(key);epoch=rng.randrange(4);cand=preferred[b]
        old_order=([cand] if cand is not None else [])+[i for i in range(256) if i!=cand]
        bits=members[b]; indexed=[]
        while bits:
            low=bits & -bits; i=low.bit_length()-1;bits-=low
            if i!=cand:indexed.append(i)
        new_order=([cand] if cand is not None else [])+indexed
        def lookup(order):
            checked=0
            for i in order:
                checked+=1
                if cache[i] and cache[i][0]==key and cache[i][1]>=epoch:return i,checked
            return None,checked
        old,oc=lookup(old_order);new,nc=lookup(new_order)
        assert old==new,(step,key,old,new)
        old_checks+=oc;new_checks+=nc
        if new is not None:preferred[b]=new
print(f'PASS: 50,000 updates/lookups with collisions, reuse, invalid epochs and clears; synthetic candidate checks {old_checks:,} -> {new_checks:,} (not a game benchmark)')
