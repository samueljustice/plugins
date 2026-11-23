import sys,re
d=open(sys.argv[1],'rb').read()
pat=sys.argv[2].encode(); maxn=int(sys.argv[3]); pre=int(sys.argv[4]); post=int(sys.argv[5])
offs=[m.start() for m in re.finditer(re.escape(pat),d)]
print(len(offs),'hits; first/last',offs[:1],offs[-1:])
import collections
gaps=collections.Counter(b-a for a,b in zip(offs,offs[1:]))
print('gaps',gaps.most_common(8))
for o in offs[:maxn]:
    s=d[o-pre:o+post]
    print(hex(o))
    for i in range(0,len(s),32):
        c=s[i:i+32]
        print('  ',c.hex(' '),' ',''.join(chr(b) if 32<=b<127 else '.' for b in c))
