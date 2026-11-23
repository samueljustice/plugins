import sys,json,os,glob
import numpy as np, librosa, soundfile as sf
mxdir,out=sys.argv[1],sys.argv[2]
res={}
for f in sorted(glob.glob(mxdir+'/*')):
    y,sr=sf.read(f,always_2d=True); y=y.mean(axis=1).astype(np.float32)
    hop=128
    env=librosa.onset.onset_strength(y=y,sr=sr,hop_length=hop,n_fft=1024)
    pk=librosa.onset.onset_detect(onset_envelope=env,sr=sr,hop_length=hop,backtrack=False,units='frames')
    bt=librosa.onset.onset_backtrack(pk,env)
    res[os.path.basename(f)]={'sr':sr,'n':len(y),'peaks':(pk*hop/sr).tolist(),'back':(bt*hop/sr).tolist(),'strength':env[pk].tolist()}
    print(os.path.basename(f),sr,len(y)/sr,len(pk))
json.dump(res,open(out,'w'))
