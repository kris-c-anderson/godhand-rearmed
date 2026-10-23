"""PS2 GS swizzle for TIM3 textures (port of anasrar/chihuahua pkg/graphic_synthesizer) + TIM3 decode/encode."""
import struct
import numpy as np

block32=[0,1,4,5,16,17,20,21,2,3,6,7,18,19,22,23,8,9,12,13,24,25,28,29,10,11,14,15,26,27,30,31]
columnWord32=[0,1,4,5,8,9,12,13,2,3,6,7,10,11,14,15]
block8=block32
columnWord8=[[0,1,4,5,8,9,12,13,0,1,4,5,8,9,12,13,2,3,6,7,10,11,14,15,2,3,6,7,10,11,14,15,
8,9,12,13,0,1,4,5,8,9,12,13,0,1,4,5,10,11,14,15,2,3,6,7,10,11,14,15,2,3,6,7],
[8,9,12,13,0,1,4,5,8,9,12,13,0,1,4,5,10,11,14,15,2,3,6,7,10,11,14,15,2,3,6,7,
0,1,4,5,8,9,12,13,0,1,4,5,8,9,12,13,2,3,6,7,10,11,14,15,2,3,6,7,10,11,14,15]]
columnByte8=[0]*8+[2]*8+[0]*8+[2]*8+[1]*8+[3]*8+[1]*8+[3]*8
Block4=[0,2,8,10,1,3,9,11,4,6,12,14,5,7,13,15,16,18,24,26,17,19,25,27,20,22,28,30,21,23,29,31]
_cw4a=[0,1,4,5,8,9,12,13]*2*2+[2,3,6,7,10,11,14,15]*2*2
_cw4b=[8,9,12,13,0,1,4,5]*2*2+[10,11,14,15,2,3,6,7]*2*2
ColumnWord4=[_cw4a+_cw4b,_cw4b+_cw4a]
ColumnByte4=([0]*8+[2]*8+[4]*8+[6]*8)*2+([1]*8+[3]*8+[5]*8+[7]*8)*2

def _addr32(w,h,dbw):
    ys,xs=np.mgrid[0:h,0:w]
    pageX=xs//64; pageY=ys//32; page=pageX+pageY*dbw
    px=xs-pageX*64; py=ys-pageY*32
    bX=px//8; bY=py//8; block=np.array(block32)[bX+bY*8]
    bx=px-bX*8; by=py-bY*8; col=by//2; cy=by-col*2
    cw=np.array(columnWord32)[bx+cy*8]
    return (page*2048+block*64+col*16+cw)  # word index

def _addr8(w,h,dbw):
    dbw>>=1
    ys,xs=np.mgrid[0:h,0:w]
    pageX=xs//128; pageY=ys//64; page=pageX+pageY*dbw
    px=xs-pageX*128; py=ys-pageY*64
    bX=px//16; bY=py//16; block=np.array(block8)[bX+bY*8]
    bx=px-bX*16; by=py-bY*16; col=by//4; cy=by-col*4
    cw=np.array(columnWord8)[col&1, bx+cy*16]
    cb=np.array(columnByte8)[bx+cy*16]
    return (page*2048+block*64+col*16+cw)*4+cb  # byte index

def _addr4(w,h,dbw):
    dbw>>=1
    ys,xs=np.mgrid[0:h,0:w]
    pageX=xs//128; pageY=ys//128; page=pageX+pageY*dbw
    px=xs-pageX*128; py=ys-pageY*128
    bX=px//32; bY=py//16; block=np.array(Block4)[bX+bY*4]
    bx=px-bX*32; by=py-bY*16; col=by//4; cy=by-col*4
    cw=np.array(ColumnWord4)[col&1, bx+cy*32]
    cb=np.array(ColumnByte4)[bx+cy*32]
    return (page*2048+block*64+col*16+cw)*8+cb  # nibble index

def unswizzle4(data,w,h):
    nib=np.zeros(len(data)*2,np.uint8); d=np.frombuffer(data,np.uint8)
    rrw,rrh=w//2,h//4
    mem=np.zeros(max(len(data)*4, 4*1024*1024),np.uint8)
    wa=_addr32(rrw,rrh,max(1,rrw//64)).ravel()
    d32=d.view('<u4')
    mem32=mem.view('<u4'); mem32[wa]=d32
    a4=_addr4(w,h,max(1,w//64)).ravel()
    memn=np.zeros(len(mem)*2,np.uint8); memn[0::2]=mem&15; memn[1::2]=mem>>4
    return memn[a4].reshape(h,w)

def swizzle4(idx,w,h):
    idx=np.asarray(idx,np.uint8).ravel()
    mem=np.zeros(4*1024*1024,np.uint8); memn=np.zeros(len(mem)*2,np.uint8)
    a4=_addr4(w,h,max(1,w//64)).ravel(); memn[a4]=idx
    mem=(memn[0::2]|(memn[1::2]<<4)).astype(np.uint8)
    rrw,rrh=w//2,h//4
    wa=_addr32(rrw,rrh,max(1,rrw//64)).ravel()
    return mem.view('<u4')[wa].astype('<u4').tobytes()

def unswizzle8(data,w,h):
    d=np.frombuffer(data,np.uint8); rrw,rrh=w//2,h//2
    mem=np.zeros(4*1024*1024,np.uint8)
    wa=_addr32(rrw,rrh,max(1,rrw//64)).ravel(); mem.view('<u4')[wa]=d.view('<u4')
    return mem[_addr8(w,h,max(1,w//64)).ravel()].reshape(h,w)

def swizzle8(idx,w,h):
    mem=np.zeros(4*1024*1024,np.uint8); mem[_addr8(w,h,max(1,w//64)).ravel()]=np.asarray(idx,np.uint8).ravel()
    rrw,rrh=w//2,h//2
    return mem.view('<u4')[_addr32(rrw,rrh,max(1,rrw//64)).ravel()].astype('<u4').tobytes()

def tim3_decode(t):
    p=16
    total,clut,img=struct.unpack_from('<3I',t,p); hs,cc=struct.unpack_from('<2H',t,p+12)
    ct,it=t[p+18],t[p+19]; w,h=struct.unpack_from('<2H',t,p+20)
    data=t[p+hs:p+hs+img]; pal=np.frombuffer(t[p+hs+img:p+hs+img+clut],np.uint8).reshape(-1,4).copy()
    sw = w>=128 and h>=128
    if it==4:
        if sw: ind=unswizzle4(data,w,h)
        else:
            d=np.frombuffer(data,np.uint8); ind=np.zeros(len(d)*2,np.uint8); ind[0::2]=d&15; ind[1::2]=d>>4; ind=ind.reshape(h,w)
    elif it==5:
        ind=unswizzle8(data,w,h) if sw else np.frombuffer(data,np.uint8).reshape(h,w)
        # CSM1 palette twiddle
        tw=np.zeros_like(pal)
        for i in range(0,256,32):
            tw[i:i+8]=pal[i:i+8]; tw[i+8:i+16]=pal[i+16:i+24]; tw[i+16:i+24]=pal[i+8:i+16]; tw[i+24:i+32]=pal[i+24:i+32]
        pal=tw
    else: raise ValueError('image type %d'%it)
    rgba=pal[ind].copy(); rgba[...,3]=np.minimum(255,rgba[...,3].astype(int)*2)
    return rgba, dict(w=w,h=h,it=it,tex0=struct.unpack_from('<Q',t,p+24)[0],tex1=struct.unpack_from('<Q',t,p+32)[0],regs=t[p+40:p+48],head=t[:16],pich=t[p:p+hs])
