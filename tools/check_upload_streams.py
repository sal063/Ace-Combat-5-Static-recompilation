import random
class Vif:
    def __init__(self):
        self.wait=0; self.cmd=0; self.acc=[]; self.last=[0]*4
        self.lane=0; self.gif=[]; self.commands=[]; self.bulk=0
    def word(self,w):
        self.lane=(self.lane+1)&3
        if not self.wait:
            self.cmd=(w>>24)&127; self.commands.append(w); self.acc=[]
            if self.cmd in (0x50,0x51): self.wait=(w&65535 or 65536)*4
        else:
            self.wait-=1; self.acc.append(w)
            self.last[len(self.acc)-1]=w
            if len(self.acc)==4 or not self.wait:
                q=self.acc+[0]*(4-len(self.acc)); self.last=q[:]
                self.gif.append(tuple(q)); self.acc=[]
    def transfer(self,words,fast):
        i=0
        while i<len(words):
            if fast and self.cmd in (0x50,0x51) and self.wait>=4 and not self.acc:
                n=min(self.wait//4,(len(words)-i)//4)
                self.wait-=n*4; self.lane=0
                self.gif.extend(tuple(words[k:k+4]) for k in range(i,i+n*4,4))
                self.last=words[i+n*4-4:i+n*4];i+=n*4;self.bulk+=1
            else:
                self.lane=0
                for w in words[i:i+4]: self.word(w)
                i+=4
    def state(self): return self.wait,self.cmd,self.acc,self.last,self.lane,self.gif,self.commands
runs=0
for seed in range(200):
    rng=random.Random(seed); words=[]
    for j in range(40):
        words += [0]*rng.randrange(8)
        n=rng.randrange(1,130)
        words.append((rng.choice((0x50,0x51))<<24)|n)
        words += [rng.getrandbits(32) for _ in range(n*4)]
    words += [0]*(-len(words)%4)
    ref=Vif();fast=Vif();pos=0
    while pos<len(words):
        end=min(len(words),pos+4*rng.randrange(1,70)); chunk=words[pos:end]
        ref.transfer(chunk,False); fast.transfer(chunk,True)
        assert ref.state()==fast.state(),(seed,pos)
        pos=end
    runs+=fast.bulk
assert runs>1000
for seed in range(200):
    rng=random.Random(seed); stream=[]
    for j in range(50):
        n=rng.randrange(0,100); image=rng.choice((False,True))
        stream.append(('tag',n,image))
        stream += [('pixel' if image else 'reg',rng.getrandbits(64),rng.getrandbits(64)) for _ in range(n)]
    left=0; image=False; got=[]; pos=0
    while pos<len(stream):
        end=min(len(stream),pos+rng.randrange(1,50))
        while pos<end:
            if left and image:
                n=min(left,end-pos); got.extend(stream[pos:pos+n]);left-=n;pos+=n
            elif left:
                got.append(stream[pos]);left-=1;pos+=1
            else:
                _,left,image=stream[pos];pos+=1
    assert got==[q for q in stream if q[0]!='tag']
print(f'PASS: 200 VIF and 200 GIF sequences; {runs} bulk DIRECT runs; split buffers, unaligned command starts, exact accumulator state and payload order')
