import sys,struct,re
d=open(sys.argv[1],'rb').read()
for name,v in [('58:00',167040000),('59:00',169920000),('1:00:00',172800000),('59:58',172704000),('58:00@47952',166873128),('1h@47.952',172627200)]:
    for w in (4,8):
        pat=struct.pack('<Q',v)[:w] if w==8 else struct.pack('<I',v)
        offs=[m.start() for m in re.finditer(re.escape(pat),d)]
        print(name,w,len(offs),[hex(o) for o in offs[:12]])
