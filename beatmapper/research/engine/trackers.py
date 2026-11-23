import sys,json,time,os
import numpy as np, librosa
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from engine import load,SR
S=sys.argv[1]
pairs=json.load(open(S+'/proto/pairs.json'))
out={}
try:
    from beat_this.inference import File2Beats
    f2b=File2Beats(checkpoint_path="final0",device="cpu",dbn=False)
except Exception as e:
    print('beat_this failed',repr(e)); f2b=None
for p in pairs:
    path=S+'/session-study/mx/'+p['file']; y=load(path)
    t=time.time(); tempo,b=librosa.beat.beat_track(y=y,sr=SR,hop_length=480,units='time'); tl=time.time()-t
    r={'librosa':list(map(float,b)),'librosa_s':tl}
    if f2b:
        try:
            t=time.time(); bb,db=f2b(path); r['beat_this']=list(map(float,bb)); r['beat_this_down']=list(map(float,db)); r['beat_this_s']=time.time()-t
        except Exception as e: print('bt run failed',repr(e))
    out[p['tag']]=r
    print(p['tag'],'librosa',len(b),float(np.atleast_1d(tempo)[0]),'%.1fs'%tl,'beat_this',len(r.get('beat_this',[])),'%.1fs'%r.get('beat_this_s',0), 60/np.median(np.diff(r.get('beat_this',[0,1]))))
json.dump(out,open(S+'/proto/beats.json','w'))
