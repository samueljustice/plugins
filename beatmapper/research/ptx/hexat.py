import sys
d=open(sys.argv[1],'rb').read()
o=int(sys.argv[2],0); n=int(sys.argv[3],0)
s=d[o:o+n]
for i in range(0,len(s),32):
    c=s[i:i+32]
    print('%08x'%(o+i),c.hex(' '),' ',''.join(chr(b) if 32<=b<127 else '.' for b in c))
