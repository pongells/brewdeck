import sys, glob, struct
from PIL import Image
for p in glob.glob(sys.argv[1]+'/*.raw'):
    d=open(p,'rb').read()
    im=Image.new('RGB',(800,480))
    px=im.load()
    v=struct.unpack('<%dH'%(800*480),d)
    for i,c in enumerate(v):
        r=(c>>11)&31; g=(c>>5)&63; b=c&31
        px[i%800,i//800]=((r*255+15)//31,(g*255+31)//63,(b*255+15)//31)
    im.save(p.replace('.raw','.png'))
    print(p.replace('.raw','.png'))
