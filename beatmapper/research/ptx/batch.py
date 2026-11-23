import sys,os,glob,json,hashlib
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from tempo import parse
def unxor(d):
    xt,xv=d[0x12],d[0x13]
    mul,neg={1:(53,False),5:(11,True)}[xt]
    for i in range(256):
        if (i*mul)&0xff==xv: delta=(-i if neg else i)&0xff;break
    key=[(i*delta)&0xff for i in range(256)]
    tabs={}
    out=[d[:0x14]];pos=0x14;n=len(d)
    while pos<n:
        blk=pos>>12;end=min(n,(blk+1)<<12);k=key[blk&0xff]
        if k not in tabs: tabs[k]=bytes(b^k for b in range(256))
        out.append(d[pos:end].translate(tabs[k]));pos=end
    return b''.join(out)
res={}
for f in sys.argv[2:]:
    try:
        d=unxor(open(f,'rb').read())
        r=parse(d)
        evs=[(e[2],e[3],e[5],e[0],e[1]) for e in r[0][3]] if r else []
        res[os.path.basename(f)]={'mtime':os.path.getmtime(f),'nblocks':len(r),'evs':evs}
        print(os.path.basename(f),len(evs),flush=True)
    except Exception as ex:
        print('ERR',f,ex,flush=True)
json.dump(res,open(sys.argv[1],'w'))
