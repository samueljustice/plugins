import sys,json,os
import numpy as np
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from tempo import parse
ons=json.load(open(sys.argv[1])); maps=json.load(open(sys.argv[2]))
d=open(sys.argv[3],'rb').read()
maps['MAIN']={'mtime':9e12,'evs':[(e[2],e[3],e[5],e[0],e[1]) for e in parse(d)[0][3]]}
clusters={}
for name,v in sorted(maps.items(),key=lambda kv:kv[1]['mtime']):
    evs=[e for e in v['evs'] if e[3]==1]
    cur=[]
    def flush():
        if len(cur)>=5:
            key=tuple(e[0] for e in cur)
            clusters.setdefault(key,(name,list(cur)))
    for e in evs:
        if cur and (e[0]-cur[-1][0])/48000>20: flush(); cur=[]
        cur.append(e)
    flush()
print(len(clusters),'distinct clusters (>=5 events)')
json.dump([{'file':n,'evs':c} for n,c in clusters.values()],open(sys.argv[4],'w'))
for key,(name,c) in clusters.items():
    S=np.array(key)/48000.0
    best=None
    for fn,o in ons.items():
        for kind in ('peaks','back'):
            O=np.array(o[kind])
            M=(S[:,None]-O[None,:]).ravel()
            M=M[(M>S[0]-o['n']/o['sr'])&(M<S[-1])]
            if len(M)==0: continue
            h,edges=np.histogram(M,bins=np.arange(M.min(),M.max()+0.01,0.005))
            h2=h[:-1]+h[1:]; i=h2.argmax(); m0=edges[i+1]
            sel=M[np.abs(M-m0)<0.012]; m=np.median(sel)
            r=S[:,None]-m-O[None,:]; rr=r[np.arange(len(S)),np.abs(r).argmin(axis=1)]
            inside=(S-m>=-0.05)&(S-m<=o['n']/o['sr']+0.05)
            hit=np.abs(rr)<0.020
            sc=(hit&inside).sum()
            if best is None or sc>best[0]: best=(sc,fn,kind,m,rr,inside,hit)
    sc,fn,kind,m,rr,inside,hit=best
    a=rr[hit&inside]*1000
    print('\ncluster first=%.3fs n=%d bpm~%.1f (from %s)'%(S[0],len(S),np.median([e[2] for e in c]),name))
    print('  best: %s [%s] music start on timeline=%.4fs (%d smp): %d/%d events within 20ms of an onset (%d inside file span); hits: median signed %.1f ms, median |.| %.1f ms, max |.| %.1f'%(fn,kind,m,round(m*48000),sc,len(S),inside.sum(),np.median(a) if len(a) else float('nan'),np.median(np.abs(a)) if len(a) else float('nan'),np.abs(a).max() if len(a) else float('nan')))
    print('  residuals ms:',' '.join('%.0f'%(x*1000) if abs(x)<0.2 else 'far' for x in rr))
