import re
from pathlib import Path

SK=49
BINS=40
COL=re.compile(r'^\[TENSOR\]\s+col(\d+):\s+(.+)$', re.M)

def load(p):
    t=Path(p).read_text()
    cols={}
    for m in COL.finditer(t):
        cols[int(m.group(1))]=[int(x) for x in m.group(2).split()]
    return [cols[i] for i in range(SK)]

def stats(mat):
    flat=[v for c in mat for v in c]
    return min(flat), max(flat), sum(flat)/len(flat)

def col_energy(mat, c):
    return sum(abs(x-100) for x in mat[c])

def speech_region(mat):
    e=[col_energy(mat,c) for c in range(SK)]
    active=[c for c,e in enumerate(e) if e>400]
    return active, e

for name in ['shot_yes_1','shot_yes_2','shot_no_1','shot_no_2']:
    p=f'{name}.txt'
    if not Path(p).exists(): continue
    m=load(p)
    act,e=speech_region(m)
    sat=sum(1 for v in (v for c in m for v in c) if v>=127)
    clip=sum(1 for v in (v for c in m for v in c) if v<=-60)
    print(f'\n=== {name} ===')
    print(f'global min/max/mean: {stats(m)}')
    print(f'saturated(127): {sat}  very_neg(<=-60): {clip}')
    print(f'active cols (L1 vs 100): {act[0]}..{act[-1]} n={len(act)}')
    print(f'col35 mean={sum(m[35])/BINS:.1f} col44 mean={sum(m[44])/BINS:.1f}')
    # compare yes1 vs no1
yes=load('shot_yes_1.txt')
no=load('shot_no_1.txt')
diff=0
for c in range(31,43):
    for b in range(BINS):
        if yes[c][b]!=no[c][b]: diff+=1
print(f'\nyes_1 vs no_1: {diff}/400 speech cells differ')
# cosine-ish on speech cols flattened
import math
yv=[yes[c][b] for c in range(31,43) for b in range(BINS)]
nv=[no[c][b] for c in range(31,43) for b in range(BINS)]
dot=sum(a*b for a,b in zip(yv,nv))
ny=math.sqrt(sum(x*x for x in yv))
nn=math.sqrt(sum(x*x for x in nv))
print(f'speech block correlation: {dot/(ny*nn):.3f}')