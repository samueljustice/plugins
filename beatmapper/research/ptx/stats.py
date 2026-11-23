import sys,json,collections
import numpy as np
cl=json.load(open(sys.argv[1]))
seen=set(); U=[]
for c in cl:
    k=tuple(round(e[2],3) for e in c['evs'])
    if k in seen: continue
    seen.add(k); U.append(c)
print(len(U),'clusters after removing time-shifted duplicates')
gaps=collections.Counter(); frac=collections.Counter(); allg=[]; dev=[]; nd=[]; perbar=[]; firsts=[]; down=0; tot=0; gsec=[]
for c in U:
    e=c['evs']; b=np.array([x[1] for x in e])/960000; bpm=np.array([x[2] for x in e]); s=np.array([x[0] for x in e])/48000
    rel=b-b[0]
    g=np.diff(b); allg+=list(g); gsec+=list(np.diff(s))
    for x in g: gaps[round(x*2)/2 if abs(x*2-round(x*2))<0.02 else 'other']+=1
    for x in rel[1:]:
        f=x-np.floor(x)
        frac['whole beat' if min(f,1-f)<0.01 else 'half beat' if abs(f-0.5)<0.01 else 'other']+=1
    med=np.median(bpm[:-1]) if len(bpm)>2 else np.median(bpm)
    dev+=list(100*(bpm[:-1]-med)/med); nd+=list(np.abs(np.diff(bpm[:-1])))
    perbar.append(len(e)/((b[-1]-b[0])/4))
    integer=abs(b[0]-round(b[0]))<0.01
    if integer:
        firsts.append((int(b[0]//4)+1, b[0]%4+1, round(s[0],2), c['file'][:40]))
        for x in b:
            if abs(x-round(x))<0.01:
                tot+=1; down+= (round(x)%4==0)
    print('  %-44s first %.2fs n=%2d span %.1f beats / %.1fs  medBPM %.2f  bpm min/max %.1f/%.1f  ev/bar %.2f'%(c['file'][:44],s[0],len(e),b[-1]-b[0],s[-1]-s[0],med,bpm[:-1].min(),bpm[:-1].max(),perbar[-1]))
allg=np.array(allg); dev=np.array(dev); nd=np.array(nd); gsec=np.array(gsec)
print('total gaps',len(allg))
print('gap (beats) histogram:',sorted(((k,v) for k,v in gaps.items() if k!='other'))[:30],'other',gaps['other'])
print('gap beats: median %.2f mean %.2f p10 %.2f p90 %.2f; gap seconds: median %.2f p10 %.2f p90 %.2f'%(np.median(allg),allg.mean(),np.percentile(allg,10),np.percentile(allg,90),np.median(gsec),np.percentile(gsec,10),np.percentile(gsec,90)))
print('position relative to cluster first event:',dict(frac))
print('bpm deviation from cluster median (%%): median abs %.2f, p75 %.2f, p90 %.2f, p95 %.2f, max %.1f; share within 1%%: %.0f%%, within 3%%: %.0f%%'%(np.median(abs(dev)),np.percentile(abs(dev),75),np.percentile(abs(dev),90),np.percentile(abs(dev),95),abs(dev).max(),100*(abs(dev)<1).mean(),100*(abs(dev)<3).mean()))
print('neighbour bpm abs diff: median %.2f p75 %.2f p90 %.2f max %.1f'%(np.median(nd),np.percentile(nd,75),np.percentile(nd,90),nd.max()))
print('events per bar: median %.2f min %.2f max %.2f'%(np.median(perbar),min(perbar),max(perbar)))
print('first events (bar, beat, sec, file):',firsts)
print('share of whole-beat events on beat 1 (clusters with integer first beat): %d/%d'%(down,tot))
