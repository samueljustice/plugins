import sys,struct,re
ZERO=0xE8D4A51000
def parse(d):
    out=[]
    for m in re.finditer(rb'\x5a\x03\x00....\x28\x20Tempo',d,re.S):
        o=m.start(); size=struct.unpack_from('<I',d,o+3)[0]
        blk=d[o:o+7+size]
        n=struct.unpack_from('<I',blk,20)[0]
        evs=[]
        for c in re.finditer(rb'TMS\x01\x00\x14\x00\x00\x00',blk):
            p=c.end()
            f0,f1=blk[p],blk[p+1]
            smp,tick=struct.unpack_from('<QQ',blk,p+2)
            g=struct.unpack_from('<H',blk,p+18)[0]
            bpm,=struct.unpack_from('<d',blk,p+20)
            unit,=struct.unpack_from('<Q',blk,p+28)
            tail=blk[p+36:p+41]
            evs.append((f0,f1,smp,tick-ZERO,g,bpm,unit,tail.hex()))
        out.append((o,size,n,evs,blk[-40:]))
    return out
if __name__=='__main__':
    d=open(sys.argv[1],'rb').read()
    r=parse(d)
    print(len(r),'tempo blocks', [(hex(o),s,n,len(e)) for o,s,n,e,_ in r], 'identical' if len(r)>1 and r[0][3]==r[1][3] else '')
    o,s,n,evs,tailb=r[0]
    print('tail',tailb.hex(' '))
    prev=None
    for e in evs:
        f0,f1,smp,tk,g,bpm,unit,tail=e
        beats=tk/960000; bar=int(beats//4)+1; beat=beats-(bar-1)*4+1
        extra=''
        if prev:
            db=(tk-prev[3])/960000; ds=smp-prev[2]
            extra='dbeats=%.4f dsamp=%d impliedBPM=%.4f'%(db,ds,db*60*48000/ds)
        print('f=%d%d smp=%10d t=%9.4fs tick=%12d beats=%10.4f bar=%d|%.3f bpm=%9.4f unit=%d g=%d %s  %s'%(f0,f1,smp,smp/48000,tk,beats,bar,beat,bpm,unit,g,tail,extra))
        prev=e
