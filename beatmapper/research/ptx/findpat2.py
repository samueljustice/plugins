import sys,struct,re
d=open(sys.argv[1],'rb').read()
for name,v in [('58:00pd',167207040),('59:00pd',170089920),('1hpd',172972800),('59:58pd',172876704)]:
    pat=struct.pack('<I',v)
    offs=[m.start() for m in re.finditer(re.escape(pat),d)]
    print(name,len(offs),[hex(o) for o in offs[:10]])
