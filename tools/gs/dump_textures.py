"""Textures of a PS2X_GIF_DUMP (F10) field, with their PCSX2 replacement names, alpha state and screen box.

    python -I tools/gs/dump_textures.py <gif_*.bin> [<pack file list>]

Replays the field's uploads into the VRAM snapshot (the game uploads character textures every frame, so the snapshot
alone is stale), then for every 4-bit texture used by a draw: the PCSX2 replacement name (as pcsx2tex.py computes it
from a TIM3), the vertex and CLUT alphas of its draws, blend state, screen box (XYOFFSET not removed). With a file list
(one name per line) marks the textures a pack replaces. Needs numpy and xxhash. Written for the 2026-10-23 transparency
/ heat-haze investigation (the answer was in the Z buffer: tools/gs/gif_dump.py lists the depth-of-field chain).
"""
import sys, struct
HERE=__import__('os').path.dirname(__import__('os').path.abspath(__file__)); sys.path.insert(0,HERE); sys.path.insert(0,HERE+'/../models'); sys.path.insert(0,HERE+'/../models/xavier')
import gif_dump as G, numpy as np, xxhash
from ghtex import _addr4, _addr8, _addr32
dump=sys.argv[1]; packlist=sys.argv[2] if len(sys.argv)>2 else None
vram=None
for k,p in G.records(dump):
    if k=='M': vram=np.frombuffer(p,np.uint8).copy(); break
draws=[]; transfers=0
def tex_indices(tbp,tbw,w,h):
    a4=_addr4(w,h,tbw).ravel()+tbp*256*2
    b=vram[a4//2]; return np.where(a4&1, b>>4, b&15).astype(np.uint8).reshape(h,w)
def clut(cbp,csa):
    a=_addr32(8+8*(csa%2),2+2*(csa//2),1)[2*(csa//2):,8*(csa%2):].ravel()*4+cbp*256
    return vram.view(np.uint8)[np.add.outer(a,np.arange(4))].astype(np.uint8).reshape(-1,4)
cache={}
def hash_tex(key):
    tbp,tbw,tw,th,cbp,csa,tcc=key
    if cache.get(key,(None,))[0]==transfers: return cache[key][1]
    w,h=1<<tw,1<<th
    ind=tex_indices(tbp,tbw,w,h)
    a4=_addr4(w,h,tbw); memn=np.zeros(int(a4.max())+1+512,np.uint8); memn[a4.ravel()]=ind.ravel()
    mem=(memn[0::2]|(memn[1::2]<<4)).astype(np.uint8)
    hs=xxhash.xxh3_64()
    for by in range(h//16):
        for bx in range(w//32):
            a=int(a4[by*16,bx*32])//2//256*256; hs.update(mem[a:a+256].tobytes())
    pal=clut(cbp,csa); bits=0x14|(tw<<6)|(th<<10)|(0x4000 if tcc else 0)
    name='%x-%x-%08x'%(hs.intdigest(),xxhash.xxh3_64_intdigest(pal.tobytes()),bits)
    res=(name,sorted(set(int(x) for x in pal[:,3])),ind,pal)
    cache[key]=(transfers,res); return res
class Gs2(G.Gs):
    def image(self,data):
        global transfers
        b,p,r=self.bitblt,self.trxpos,self.trxreg
        dbp=(b>>32)&0x3FFF; dbw=(b>>48)&0x3F; dpsm=(b>>56)&0x3F; dx=(p>>32)&0x7FF; dy=(p>>48)&0x7FF; w=r&0xFFF; h=(r>>32)&0xFFF
        if not hasattr(self,'_img'): self._img=bytearray()
        self._img+=data
        need={0:4,0x13:1,0x14:0.5}.get(dpsm)
        if need is None: return
        if len(self._img)>=int(w*h*need):
            d=np.frombuffer(bytes(self._img[:int(w*h*need)]),np.uint8); self._img=bytearray()
            transfers+=1
            if dpsm==0:
                a=_addr32(dx+w,dy+h,max(1,dbw))[dy:dy+h,dx:dx+w].ravel()*4+dbp*256
                vram[np.add.outer(a,np.arange(4)).ravel()]=d
            elif dpsm==0x13:
                a=_addr8(dx+w,dy+h,max(1,dbw))[dy:dy+h,dx:dx+w].ravel()+dbp*256
                vram[a]=d
            elif dpsm==0x14:
                a=_addr4(dx+w,dy+h,max(1,dbw))[dy:dy+h,dx:dx+w].ravel()+dbp*512
                lo=np.zeros(len(d)*2,np.uint8); lo[0::2]=d&15; lo[1::2]=d>>4
                byte=a//2; hi=(a&1).astype(bool)
                cur=vram[byte]; vram[byte]=np.where(hi,(cur&0x0F)|(lo<<4),(cur&0xF0)|lo)
def out(field,index,d):
    if d.get('transfer'): return
    t=d['tex0']
    if not (d['prim']>>4)&1: return
    psm=(t>>20)&0x3F
    if psm!=0x14: return
    key=(t&0x3FFF,(t>>14)&0x3F,(t>>26)&0xF,(t>>30)&0xF,(t>>37)&0x3FFF,(t>>56)&0x1F,(t>>34)&1)
    name,cla,ind,pal=hash_tex(key)
    xs=[v[0] for v in d['verts']]; ys=[v[1] for v in d['verts']]
    draws.append(dict(field=field,index=index,name=name,key=key,n=len(d['verts']),alphas=sorted(set(v[3][3] for v in d['verts'])),abe=(d['prim']>>6)&1,alpha=d['alpha']&0xFFFFFFFF,clut_alpha=cla,bbox=(min(xs),min(ys),max(xs),max(ys)),test=d['test']&0xFFFFF,frame=d['frame']&0xFFFFFFFF))
gs=Gs2(out); stream=bytearray()
for kind,payload in G.records(dump):
    if kind=='G': stream+=payload
    elif kind=='V':
        G.parse_gif(gs,bytes(stream)); gs.flush(); stream.clear(); gs.field+=1; gs.index=0
        if gs.field>=1: break   # one field is enough
print('draws',len(draws),'transfers',transfers)
pack=set(l.strip().rsplit('.',1)[0] for l in open(packlist)) if packlist else set()
from collections import defaultdict
agg=defaultdict(lambda: dict(n=0,verts=0,alphas=set(),abe=set(),clut=None,bbox=[1e9,1e9,-1e9,-1e9],key=None))
for dr in draws:
    e=agg[dr['name']]; e['n']+=1; e['verts']+=dr['n']; e['alphas']|=set(dr['alphas']); e['abe'].add((dr['abe'],hex(dr['alpha']))); e['clut']=dr['clut_alpha']; e['key']=dr['key']
    b=e['bbox']; e['bbox']=[min(b[0],dr['bbox'][0]),min(b[1],dr['bbox'][1]),max(b[2],dr['bbox'][2]),max(b[3],dr['bbox'][3])]
for name,e in sorted(agg.items(),key=lambda kv:-kv[1]['verts']):
    k=e['key']; print('PACK' if name in pack else '    ',name,'%dx%d'%(1<<k[2],1<<k[3]),'draws',e['n'],'verts',e['verts'],'vtx alpha',sorted(e['alphas']),'abe',sorted(e['abe']),'clut alpha',e['clut'],'bbox',[round(x) for x in e['bbox']],'tbp',hex(k[0]))
