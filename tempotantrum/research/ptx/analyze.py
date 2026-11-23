import sys,json
res=json.load(open(sys.argv[1]))
seen={}
for name,v in sorted(res.items(),key=lambda kv:kv[1]['mtime']):
    key=json.dumps(v['evs'])
    seen.setdefault(key,[]).append(name)
def tc(s):
    return '%d:%05.2f'%(s//60,s%60)
k=0
for key,names in seen.items():
    evs=json.loads(key); k+=1
    print('\n=== MAP %d: %d events (incl. end sentinel); files: %s .. %s (%d)'%(k,len(evs),names[0],names[-1],len(names)))
    prev=None
    for smp,tk,bpm,f0,f1 in evs:
        t=smp/48000; beats=tk/960000
        bar=int(beats//4)+1; beat=beats-(bar-1)*4+1
        gap='' if prev is None else ' +%.3f beats +%.2fs'%(beats-prev[1]/960000,(smp-prev[0])/48000)
        print('  %s f=%d%d t=%9.3fs (%s) beats=%9.3f bar %4d|%.3f bpm=%8.3f%s'%('END' if f0==0 else '   ',f0,f1,t,tc(t),beats,bar,beat,bpm,gap))
        prev=(smp,tk)
