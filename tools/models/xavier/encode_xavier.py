import os
WORK=os.environ.get('XAVIER_WORK','xavier_work')
"""Xavier faces JSON + textures -> God Hand node blobs (scr) + TIM3 textures. numpy + Pillow."""
import json, math, os, struct, sys
import numpy as np
from PIL import Image
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__))); sys.path.insert(0,os.path.join(os.path.dirname(os.path.abspath(__file__)),'..'))
from ghmodel import *
from ghtex import swizzle4
from strip import build_buffers

OUT=WORK; TEX=WORK+'/textures'
J=json.load(open(OUT+'/xavier_faces.json'))
faces=J['faces']

from collections import Counter
cnt=Counter(f['mat'] for f in faces)
print(cnt)
fur='body0'
others=sorted([m for m in cnt if m.startswith('body') and m!=fur], key=lambda m:-cnt[m])
print('others',others)
# The texture slots keep the game's own sizes and order (pl0000..pl0010, pl0004a, pl0005a): Xavier's art in Gene's
# shapes. Each slot: (tm3 index, name, width, height, alpha, [(material, source image, rotate 90, sub-rect x0,y0,x1,y1)])
def green(m):
    fl=[f for f in faces if f['mat']==m]; A=np.asarray(Image.open(os.path.join(TEX,'XavierDiffuse.png')).convert('RGB')).astype(float)
    H,W=A.shape[:2]; c=np.array([A[min(H-1,int((1-cc['uv'][1])*H)),min(W-1,int(cc['uv'][0]*W))] for f in fl for cc in f['c']])
    return (c[:,1]-(c[:,0]+c[:,2])/2).mean()
assert len(others)==2
snake,shoes=sorted(others,key=green,reverse=True)
SLOTS=[(0,'pl0000',128,256,False,[('head','XavierFace.png',True,(0,0,1,1))]),
       (1,'pl0001',256,512,False,[(fur,'XavierDiffuse.png',False,(0,0,1,1))]),
       (2,'pl0002',128,256,False,[(snake,'XavierDiffuse.png',False,(0,0,1,1))]),
       (3,'pl0003',128,128,False,[('loin','XavierDiffuse.png',False,(0,0,1,1))]),
       (6,'pl0006',64,64,False,[(shoes,'XavierDiffuse.png',False,(0,0,1,1))]),
       (7,'pl0007',128,128,True,[('hair','Xavier_HAIR.png',False,(0,0,1,1))]),
       (8,'pl0008',64,32,False,[('eyeR','Eye_BLUE.png',False,(0,0,0.5,1)),('eyeL','Eye_BROWN.png',False,(0.5,0,1,1))]),
       (9,'pl0009',64,32,False,[('snakeeye','Xavier_SNAKEEYE.png',True,(0,0,1,1))])]
plan={m[0]:sl[0] for sl in SLOTS for m in sl[5]}
node_of={'head':'HD_00','eyeR':'HD_00','eyeL':'HD_00','hair':'z_00','loin':'COAT'}

def p2(x,lo,hi):
    v=lo
    while v<x and v<hi: v*=2
    return v

def tim3_encode(rgba_idx, pal, w, h):
    """rgba_idx: HxW indices 0..15; pal: 16x4 RGBA (0..255)"""
    idx=np.asarray(rgba_idx,np.uint8)
    if w>=128 and h>=128: data=swizzle4(idx,w,h); fid=6
    else:
        f=idx.ravel(); data=bytes((f[0::2]|(f[1::2]<<4)).astype(np.uint8)); fid=0
    clut=bytearray()
    for r,g,b,a in pal: clut+=bytes((int(r),int(g),int(b),int(round(int(a)*128/255))))
    tbw=max(2,w//64); tw=int(math.log2(w)); th=int(math.log2(h))
    tex0=(tbw<<14)|(20<<20)|(tw<<26)|(th<<30)
    pic=struct.pack('<3I2H4B2H2Q2I', 48+len(clut)+len(data), len(clut), len(data), 48, 16, 0, 1, 3, 4, w, h, tex0, 0x260, 0, 0)
    return b'TIM3'+bytes((4,fid,1,0))+bytes(8)+pic+data+bytes(clut)

def raster_mask(shape, tris_px):
    from PIL import ImageDraw
    m=Image.new('L',(shape[1],shape[0]),0); d=ImageDraw.Draw(m)
    for t in tris_px: d.polygon([tuple(p) for p in t],fill=255)
    return np.asarray(m)>0

textures={}; uvf={}
for idx,nm,TW,TH,alpha,parts in SLOTS:
    canvas=np.zeros((TH,TW,4),np.uint8); mask=np.zeros((TH,TW),bool)
    for mat,src,rot,(sx0,sy0,sx1,sy1) in parts:
        fl=[f for f in faces if f['mat']==mat]
        if not fl: print('no faces for',mat); continue
        im=Image.open(os.path.join(TEX,src)).convert('RGBA'); W,H=im.size
        px=np.array([[c['uv'][0]*W,(1-c['uv'][1])*H] for f in fl for c in f['c']])
        x0,y0=np.floor(px.min(0))-2; x1,y1=np.ceil(px.max(0))+2
        x0=max(0,x0); y0=max(0,y0); x1=min(W,x1); y1=min(H,y1)
        cw,ch=x1-x0,y1-y0
        rx0,ry0=int(round(sx0*TW)),int(round(sy0*TH)); rw,rh=int(round(sx1*TW))-rx0,int(round(sy1*TH))-ry0
        crop=im.crop((int(x0),int(y0),int(x1),int(y1)))
        if rot: crop=crop.transpose(Image.Transpose.ROTATE_90)
        canvas[ry0:ry0+rh,rx0:rx0+rw]=np.asarray(crop.resize((rw,rh),Image.LANCZOS))
        def f_(u,v,W=W,H=H,x0=x0,y0=y0,cw=cw,ch=ch,rot=rot,rx0=rx0,ry0=ry0,rw=rw,rh=rh,TW=TW,TH=TH):
            tx=(u*W-x0)/cw; ty=((1-v)*H-y0)/ch
            if rot: tx,ty=ty,1-tx   # PIL ROTATE_90 (counter-clockwise): (x,y) -> (y, 1-x)
            return (rx0+tx*rw)/TW, (ry0+ty*rh)/TH
        uvf[mat]=f_
        tris=[[(f_(*c['uv'])[0]*TW, f_(*c['uv'])[1]*TH) for c in f['c']] for f in fl]
        mask|=raster_mask((TH,TW),tris)
        print('TEX',mat,'->',idx,nm,TW,'x',TH,'part',rw,'x',rh,'from',int(cw),'x',int(ch),'rot' if rot else '')
    arr=canvas
    sel=arr[mask] if mask.sum()>32 else arr.reshape(-1,4)
    if alpha:
        q=Image.fromarray(sel.reshape(1,-1,4),'RGBA').quantize(16,method=Image.Quantize.FASTOCTREE)
        pal=np.array(q.getpalette(rawmode='RGBA')[:64],np.uint8).reshape(-1,4)
    else:
        q=Image.fromarray(sel[:,:3].reshape(1,-1,3),'RGB').quantize(16,method=Image.Quantize.MEDIANCUT,dither=Image.Dither.NONE)
        pal3=np.array(q.getpalette()[:48],np.uint8).reshape(-1,3); pal=np.hstack([pal3,np.full((len(pal3),1),255,np.uint8)])
    while len(pal)<16: pal=np.vstack([pal,pal[-1:]])
    pal=pal[:16]
    d=((arr[:,:,None,:].astype(int)-pal[None,None,:,:].astype(int))**2)
    if not alpha: d=d[...,:3]
    ind=np.argmin(d.sum(-1),-1)
    Image.fromarray(pal[ind].astype(np.uint8),'RGBA').save(OUT+'/tex_%s.png'%nm)
    textures[idx]=(nm,tim3_encode(ind,pal,TW,TH))

def game(v): return (v[0],v[2],-v[1])

# Gene's coat tails (pl/pl01.dat): 8 chains of 5 bones around the waist, simulated by the game. The loincloth panels
# hang from the front and back chains.
import os as _os
_b=open(_os.path.join(_os.path.dirname(os.environ['GENE_PL00']),'pl01.dat'),'rb').read()
_md=[d for t,d,o in dat_read(_b) if t=='MD'][0]; _k,_nodes=scr_read(_md); _m=mdb_read(_md,_nodes[0]['mdb_off'])
CW=[]
for bn in _m['bones']:
    p_=bn['parent']; base=CW[p_] if p_>=0 else (0,0,0); CW.append(tuple(base[j]+bn['t'][j] for j in range(3)))
COAT_TEX=[nm for nm,_,_ in tm3_read([d for t,d,o in dat_read(_b) if t=='TM3'][0])].index('pl0003')
def coat_weights(p):
    x,y,z=p
    l,r=(1,21) if z>0 else (16,36)
    t=min(1,max(0,(x-CW[l][0])/(CW[r][0]-CW[l][0])))
    top=CW[l][1]
    if y>=top: k=None
    else:
        k=0
        while k<4 and y<CW[l+k+1][1]: k+=1
    wroot=1.0 if y>=top else max(0.0,(y-(top-0.06))/0.06)
    ws=[(0,wroot)]
    if k is not None: ws+= [(l+k,(1-wroot)*(1-t)),(r+k,(1-wroot)*t)]
    ws=[w for w in ws if w[1]>0.001]
    return sorted(ws,key=lambda w:-w[1])[:3]

nodes={}
for mat in plan:
    if mat not in uvf: continue
    idx=plan[mat]
    node=node_of.get(mat,'cort')
    fl=[f for f in faces if f['mat']==mat]
    ids={}; verts=[]; tris=[]
    for f in fl:
        t=[]
        for c in f['c']:
            tx,ty=uvf[mat](*c['uv'])
            u=int(round(tx*4096)); v=int(round(-ty*4096))
            k=(tuple(c['key']),u,v)
            if k not in ids:
                ids[k]=len(verts)
                w=coat_weights(game(c['pos'])) if node=='COAT' else c['w']; tot=sum(x[1] for x in w) or 1
                pct=[int(math.floor(x[1]*100/tot)) for x in w]
                if pct: pct[0]+=100-sum(pct)
                j=[0,0,0,0]; ww=[0,0,0,0]
                for s,(b,_) in enumerate(w):
                    if pct[s]>0: j[s+1]=b*4; ww[s]=pct[s]
                n=game(c['nrm']); 
                verts.append(dict(pos=game(c['pos']),nrm=tuple(max(-127,min(127,int(round(x*127)))) for x in n),uv=(max(-32768,min(32767,u)),max(-32768,min(32767,v))),wt=(tuple(j),tuple(ww))))
            t.append(ids[k])
        if len(set(t))==3: tris.append(tuple(t))
    for buf in build_buffers(tris, 900):
        vb=dict(pos=[verts[i]['pos'] for i,_ in buf],flags=[fl_ for _,fl_ in buf],nrm=[verts[i]['nrm'] for i,_ in buf],
                uv=[verts[i]['uv'] for i,_ in buf],col=[(128,128,128,128)]*len(buf),wt=[verts[i]['wt'] for i,_ in buf],mat=COAT_TEX if node=='COAT' else idx,extra=bytes(8))
        nodes.setdefault(node,[]).append(vb)
for n,vbs in nodes.items(): print('NODE',n,len(vbs),'vbs',sum(len(v['pos']) for v in vbs),'verts')

# write the Xavier node file (scr) with an empty bone table and the texture container
HDR=bytes(16)+struct.pack('<f',1.0)
TAIL=bytes.fromhex('000000000000000001000000000000000000164400007f00')+bytes(36)
coat=nodes.pop('COAT',None)
order=sorted(nodes, reverse=True)
if coat:
    open(OUT+'/xavier_coat.scr','wb').write(scr_write(3,[dict(name='z_b',mdb=dict(vbs=coat,hdr=HDR),srt=(1,1,1,0,0,0,0,0,0),tail=TAIL)], bytes(16*41), 0))
scr=scr_write(3,[dict(name=n,mdb=dict(vbs=nodes[n],hdr=HDR),srt=(1,1,1,0,0,0,0,0,0),tail=TAIL) for n in order], bytes(16*32), 0)
open(OUT+'/xavier_nodes.scr','wb').write(scr)
tm=tm3_write([(n,t) for i,(n,t) in sorted(textures.items())])
open(OUT+'/xavier_textures.tm3','wb').write(tm)
print('scr',len(scr),'tm3',len(tm))
