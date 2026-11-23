import sys,json,time,os
import numpy as np
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
import engine as E
SR=E.SR
S=sys.argv[1]; REF=sys.argv[2] if len(sys.argv)>2 else None; VERB=len(sys.argv)>3
pairs=json.load(open(S+'/proto/pairs.json')); beats=json.load(open(S+'/proto/beats.json'))
ms=lambda x: np.asarray(x)/SR*1000
def hand(p,n):
    ev=[e for e in p['evs'] if 0<=e[0]<n]
    hs=np.array([e[0] for e in ev],float); hb=np.array([e[1] for e in ev])
    return hs,hb
def stats(d):
    a=np.abs(d); return 'med %.1f p90 %.1f max %.0f | <5ms %2.0f%% <10 %2.0f%% <20 %2.0f%%'%(np.median(a),np.percentile(a,90),a.max(),100*(a<5).mean(),100*(a<10).mean(),100*(a<20).mean())
cache={}
for p in pairs:
    y=E.load(S+'/session-study/mx/'+p['file']); t=time.time(); pk,st,_=E.flux_onsets(y); cache[p['tag']]=(y,pk,st,time.time()-t)
if REF is None:
    print('ATTACK-START TUNING: hand event minus nearest refined onset (events within 30 ms only)')
    for mode in ('flux','fluxshift','hyb','last10','last20','first20','slope','last10s','last20s','first20s'):
        for p in pairs:
            y,pk,st,_=cache[p['tag']]; on=E.refine(y,pk,mode); hs,hb=hand(p,len(y))
            d=np.array([ms(h-on[np.abs(on-h).argmin()]) for h in hs]); g=np.abs(d)<30
            print('  %-8s %-7s n=%2d/%2d signed med %+5.1f  abs med %4.1f  p90 %4.1f  <5ms %d'%(mode,p['tag'],g.sum(),len(d),np.median(d[g]),np.median(np.abs(d[g])),np.percentile(np.abs(d[g]),90),(np.abs(d)<5).sum()))
    sys.exit()
for p in pairs:
    y,pk,st,tflux=cache[p['tag']]; t=time.time(); on=E.refine(y,pk,REF); tref=time.time()-t
    hs,hb=hand(p,len(y)); b0=hb[0]
    B=np.arange(np.ceil(hb[0]),np.floor(hb[-1])+1); H=np.interp(B,hb,hs)
    print('\n=== %s  %s  dur %.1fs  onsets %d  hand events in file %d  beats scored %d (%.1f-%.1fs) hand median bpm %.1f'%(p['tag'],p['file'][:30],len(y)/SR,len(on),len(hs),len(B),H[0]/SR,H[-1]/SR,60*SR/np.median(np.diff(H))))
    runs=[]
    t=time.time(); r=E.run_hinted(y,on,st,hs[0],round(p['evs'][0][2])); r['t']=time.time()-t; r['scale']=1; r['off']=0; runs.append(('B hinted %dbpm'%round(p['evs'][0][2]),r))
    for trk in ('beat_this','librosa'):
        if trk not in beats[p['tag']]: continue
        bt=np.array(beats[p['tag']][trk])*SR
        t=time.time(); r=E.run_auto(y,on,st,bt); r['t']=time.time()-t+beats[p['tag']][trk+'_s']
        ratio=np.median(np.diff(H))/np.median(np.diff(r['tt'])); r['scale']=min((0.5,1,2,3,1.5,4),key=lambda s:abs(np.log(ratio/s)))
        best=None
        for off in range(-8,len(r['kk'])):
            d=E.map_time(r['kk'],r['tt'],r['A'],(B-b0)*r['scale']+off)-H
            m=-(np.abs(d)<0.02*SR).sum()
            if best is None or m<best[0]: best=(m,off)
        r['off']=best[1]; runs.append(('A '+trk,r))
    for name,r in runs:
        f=lambda b:E.map_time(r['kk'],r['tt'],r['A'],(np.asarray(b)-b0)*r['scale']+r['off'])
        d=ms(f(B)-H); de=ms(f(hb)-hs)
        lines=E.map_time(r['kk'],r['tt'],r['A'],np.arange(-20,len(r['kk'])+20)); dn=ms(np.array([lines[np.abs(lines-h).argmin()]-h for h in H]))
        print('   lenient (hand beat vs NEAREST auto grid line): '+stats(dn))
        inr=[a for a in r['A'] if H[0]-SR*0.01<=r['tt'][a]<=H[-1]+SR*0.01]
        print('%-18s %s | anchors %d (in hand range %d) vs hand %d | snapped %d/%d | at hand events: med %.1f p90 %.1f max %.0f | scale x%s off %+d | %.2fs'%(name,stats(d),len(r['A']),len(inr),len(hs),r['conf'].sum(),len(r['kk']),np.median(np.abs(de)),np.percentile(np.abs(de),90),np.abs(de).max(),r['scale'],r['off'],r['t']+tflux+tref))
        if True:
            idx=np.arange(-20,len(r['kk'])+20); near=np.array([idx[np.abs(lines-h).argmin()] for h in H]); slip=near-((B-b0)*r['scale']+r['off']); okk=np.abs(dn)<20
            runs_=[];
            for i in range(len(B)):
                key=(slip[i] if okk[i] else 'unlocked')
                if runs_ and runs_[-1][0]==key: runs_[-1][2]=H[i]/SR; runs_[-1][3]+=1
                else: runs_.append([key,H[i]/SR,H[i]/SR,1])
            print('   numbering runs (slip in auto beats, t0-t1, n): '+'; '.join('%s %.1f-%.1fs n=%d'%(str(k),a_,b_,n) for k,a_,b_,n in runs_ if n>=3))
            snapd=[ms(np.abs(on-x).min()) for x in H]; snapa=[ms(np.abs(on-x).min()) for x in f(B)]
            print('   beats with an onset within 5 ms: hand %d/%d, auto %d/%d'%((np.array(snapd)<5).sum(),len(B),(np.array(snapa)<5).sum(),len(B)))
        if VERB and (name.startswith('B') or 'beat_this' in name):
            bad=np.nonzero(np.abs(d)>12)[0]
            for i in bad:
                jh=np.abs(on-H[i]).argmin(); a=f(B[i:i+1])[0]; ja=np.abs(on-a).argmin()
                print('     beat %6.1f t=%6.2fs auto-hand %+6.1f ms | hand->nearest onset %+6.1f (str %.2f) | auto->nearest onset %+6.1f (str %.2f)%s'%(B[i],H[i]/SR,d[i],ms(on[jh]-H[i]),st[jh],ms(on[ja]-a),st[ja],'  [hand event]' if B[i] in hb else ''))
