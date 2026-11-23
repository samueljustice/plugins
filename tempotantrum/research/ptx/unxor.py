import sys
src,dst=sys.argv[1],sys.argv[2]
d=open(src,'rb').read()
xt,xv=d[0x12],d[0x13]
mul,neg={1:(53,False),5:(11,True)}[xt]
delta=None
for i in range(256):
    if (i*mul)&0xff==xv: delta=(-i if neg else i)&0xff;break
key=[(i*delta)&0xff for i in range(256)]
out=bytearray(d[:0x14])
n=len(d)
if xt==5:
    pos=0x14
    while pos<n:
        blk=pos>>12; end=min(n,(blk+1)<<12)
        k=key[blk&0xff]
        out+=bytes(d[pos:end]).translate(bytes(b^k for b in range(256)))
        pos=end
else:
    out+=bytes(c^key[(0x14+i)&0xff] for i,c in enumerate(d[0x14:]))
open(dst,'wb').write(out)
print('type',xt,'val',hex(xv),'delta',delta,'len',n)
