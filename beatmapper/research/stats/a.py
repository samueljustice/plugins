import json,collections,numpy as np
cl=json.load(open('session-study/clusters.json'))
seen=set();U=[]
for c in cl:
    k=tuple(round(e[2],3) for e in c['evs'])
    if k in seen: continue
    seen.add(k);U.append(c)
print(len(cl),len(U))
SR=48000
red=[];allrm=[];beatpos=collections.Counter();resid=[];tickmod=collections.Counter();short=0;shortbig=0;ng=0
lsb=collections.Counter()
print('cluster summary: file, first s, n, medBPM, span s')
for c in U:
    e=c['evs'];s=np.array([x[0] for x in e],float);t=np.array([x[1] for x in e],float);bpm=np.array([x[2] for x in e])
    b=t/960000
    print('  %-46s %7.2f n=%2d bpm %.1f span %.1f'%(c['file'][:46],s[0]/SR,len(e),np.median(bpm[:-1]),(s[-1]-s[0])/SR))
    # bpm exactness
    for i in range(len(e)-1):
        calc=(t[i+1]-t[i])/960000/((s[i+1]-s[i])/SR)*60
        resid.append(abs(calc-bpm[i])/bpm[i])
    # removal error
    for i in range(1,len(e)-1):
        pred=s[i-1]+(s[i+1]-s[i-1])*(b[i]-b[i-1])/(b[i+1]-b[i-1])
        allrm.append((pred-s[i])/SR*1000)
    for x in t: tickmod[int(x)%960000%1000==0]+=1
    for x in s:
        v=int(x);k=0
        while v%2==0 and k<10: v//=2;k+=1
        lsb[k]+=1
    med=np.median(bpm[:-1])
    for i in range(len(e)-1):
        g=b[i+1]-b[i];ng+=1
        if g<=1.01:
            short+=1
            if abs(bpm[i]-med)/med>0.03: shortbig+=1
    if abs(b[0]-round(b[0]))<0.01:
        for x in b:
            if abs(x-round(x))<0.01: beatpos[int(round(x))%4+1]+=1
            elif abs(x*2-round(x*2))<0.02: beatpos['half']+=1
            else: beatpos['other']+=1
a=np.abs(allrm)
print('interior events',len(a),'removal error ms: median %.1f p25 %.1f p75 %.1f p90 %.1f'%(np.median(a),np.percentile(a,25),np.percentile(a,75),np.percentile(a,90)))
for th in (2,4,8,15,30): print('  share removable within %d ms: %.0f%%'%(th,100*(a<th).mean()))
print('bpm rel resid max',max(resid),'median',np.median(resid))
print('tick multiple of 1000 (1/960 beat):',dict(tickmod))
print('beat positions (clusters w/ integer first beat):',dict(beatpos))
print('gaps',ng,'<=1 beat',short,'of which bpm >3% off median',shortbig)
print('sample-pos trailing zero bits:',sorted(lsb.items()))
# all gaps with >3% deviation: lengths
dl=[]
for c in U:
    e=c['evs'];t=np.array([x[1] for x in e],float)/960000;bpm=np.array([x[2] for x in e]);med=np.median(bpm[:-1])
    for i in range(len(e)-1):
        if abs(bpm[i]-med)/med>0.03: dl.append(round(t[i+1]-t[i],2))
print('segments >3%% off: n=%d, length beats median %.2f, share<=1 beat %.0f%%, <=2 beats %.0f%%'%(len(dl),np.median(dl),100*np.mean(np.array(dl)<=1.01),100*np.mean(np.array(dl)<=2.01)))
# ms shift implied by those segments: (seg dur) - (dur at median bpm)
sh=[]
for c in U:
    e=c['evs'];s=np.array([x[0] for x in e],float);t=np.array([x[1] for x in e],float)/960000;bpm=np.array([x[2] for x in e]);med=np.median(bpm[:-1])
    for i in range(len(e)-1):
        sh.append(((s[i+1]-s[i])/SR-(t[i+1]-t[i])*60/med)*1000)
sh=np.abs(sh);print('per-segment time deviation vs constant median tempo, ms: median %.1f p75 %.1f p90 %.1f; share <4ms %.0f%% <10ms %.0f%%'%(np.median(sh),np.percentile(sh,75),np.percentile(sh,90),100*(sh<4).mean(),100*(sh<10).mean()))
