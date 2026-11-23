import sys,json,os
import numpy as np
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
import engine as E
SR=E.SR; S=sys.argv[1]; REF=sys.argv[2]; which=sys.argv[3]
pairs=json.load(open(S+'/proto/pairs.json')); beats=json.load(open(S+'/proto/beats.json'))
for p in pairs:
    y=E.load(S+'/session-study/mx/'+p['file']); pk,st,_=E.flux_onsets(y); on=E.refine(y,pk,REF)
    ev=[e for e in p['evs'] if 0<=e[0]<len(y)]; hs=np.array([e[0] for e in ev],float); hb=np.array([e[1] for e in ev]); b0=hb[0]
    B=np.arange(np.ceil(hb[0]),np.floor(hb[-1])+1); H=np.interp(B,hb,hs)
    if which=='B': r=E.run_hinted(y,on,st,hs[0],round(p['evs'][0][2])); sc=1; off=0
    else:
        r=E.run_auto(y,on,st,np.array(beats[p['tag']]['beat_this'])*SR); sc=int(sys.argv[4+pairs.index(p)*2]); off=int(sys.argv[5+pairs.index(p)*2])
    a=E.map_time(r['kk'],r['tt'],r['A'],(B-b0)*sc+off); d=(a-H)/SR*1000
    print('\n===',p['tag'],which)
    for i in range(len(B)):
        jh=np.abs(on-H[i]).argmin(); ja=np.abs(on-a[i]).argmin()
        k=(B[i]-b0)*sc+off; cf=''
        if 0<=k<len(r['kk']) and k==int(k): cf='snap' if r['conf'][int(k)] else 'interp'
        flag='EV' if B[i] in hb else '  '
        if abs(d[i])>10 or flag=='EV':
            print('  b%6.1f %s t=%6.2f d=%+7.1f  hand->onset %+6.1f(s%.2f)  auto->onset %+6.1f(s%.2f) %s'%(B[i],flag,H[i]/SR,d[i],(on[jh]-H[i])/SR*1000,st[jh],(on[ja]-a[i])/SR*1000,st[ja],cf))
