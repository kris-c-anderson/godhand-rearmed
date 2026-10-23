"""God Hand model containers: dat, scr/mdb (MD), TM3/TIM3 headers; read, write, and the merge of a character mod into
the game's own player files. Standard library only (the build runs it on your disc).

Layouts from anasrar/chihuahua (pkg/*/*.hexpat) and akitotheanimator/God-Hand-Tools (Blen2MD), checked against
pl/pl00.dat: a dat is (u32 count, u32 offsets[count], char type[4][count]), entries 32-byte aligned; an MD is
"scr\0", kind 3, node count, node offsets, then per node an "mdb\0" block (bone table copy + vertex buffers) and
112-byte node records. A vertex buffer holds triangle strips: position + flag (0x8000 = restart, 0/1 = winding),
int8 normals, int16 UVs (4096 = 1.0, v negative), RGBA colour (0x80 = 1.0) and three bone weights (index*4, percent).
"""
import struct

def u32(b,o): return struct.unpack_from('<I',b,o)[0]
def i32(b,o): return struct.unpack_from('<i',b,o)[0]
def u16(b,o): return struct.unpack_from('<H',b,o)[0]
def i16(b,o): return struct.unpack_from('<h',b,o)[0]
def f32(b,o): return struct.unpack_from('<f',b,o)[0]

def dat_read(b):
    n=u32(b,0)
    offs=[u32(b,4+4*i) for i in range(n)]
    types=[b[4+4*n+4*i:8+4*n+4*i].rstrip(b'\0').decode('latin1') for i in range(n)]
    ends=sorted(set([o for o in offs if o]+[len(b)]))
    out=[]
    for o,t in zip(offs,types):
        if o==0: out.append((t,None,0)); continue
        e=min(x for x in ends if x>o)
        out.append((t,b[o:e],o))
    return out

def scr_read(b):
    assert b[:3]==b'scr', b[:4]
    kind=u32(b,4); n=u32(b,8)
    nodes=[]
    for i in range(n):
        no=u32(b,16+4*i)
        mdb_rel=i32(b,no)
        name=b[no+8:no+16].rstrip(b'\0').decode('latin1') if kind!=0 else ''
        tr=struct.unpack_from('<9f',b,no+16)
        nodes.append(dict(node_off=no,mdb_off=no+mdb_rel,name=name,unk4=u32(b,no+4),srt=tr,tail=b[no+52:no+112]))
    return kind,nodes

def mdb_read(b,o):
    assert b[o:o+3]==b'mdb'
    bone_off=o+u32(b,o+4); nb=u16(b,o+8); nv=u16(b,o+10)
    hdr=b[o+12:o+32]
    vbs=[]
    for i in range(nv):
        vo=o+u32(b,o+32+4*i)
        po,no_,uo,co,wo=struct.unpack_from('<5I',b,vo)
        cnt=u16(b,vo+20); mat=u16(b,vo+22)
        extra=b[vo+24:vo+32]
        pos=[];flags=[]
        for k in range(cnt):
            x,y,z,fl=struct.unpack_from('<3fi',b,vo+po+16*k)
            pos.append((x,y,z)); flags.append(fl)
        nrm=[tuple(struct.unpack_from('<3b',b,vo+no_+4*k)) for k in range(cnt)] if no_ else None
        uv=[struct.unpack_from('<2h',b,vo+uo+4*k) for k in range(cnt)]
        col=[tuple(b[vo+co+4*k:vo+co+4*k+4]) for k in range(cnt)] if co else None
        wt=[(tuple(b[vo+wo+8*k:vo+wo+8*k+4]),tuple(b[vo+wo+8*k+4:vo+wo+8*k+8])) for k in range(cnt)] if wo else None
        vbs.append(dict(off=vo,count=cnt,mat=mat,pos=pos,flags=flags,nrm=nrm,uv=uv,col=col,wt=wt,extra=extra,offs=(po,no_,uo,co,wo)))
    bones=[]
    for i in range(nb):
        x,y,z,mir,par=struct.unpack_from('<3fHh',b,bone_off+16*i)
        bones.append(dict(t=(x,y,z),mirror=mir,parent=par))
    return dict(bones=bones,vbs=vbs,hdr=hdr,bone_off=bone_off)

def tm3_read(b):
    assert b[:3]==b'TM3', b[:4]
    n=u32(b,4)
    offs=[u32(b,16+4*i) for i in range(n)]
    no=16+4*n+(4 if n&1 else 0)
    names=[b[no+8*i:no+8*i+8].rstrip(b'\0').decode('latin1') for i in range(n)]
    ends=sorted(set(offs+[len(b)]))
    texs=[]
    for o,nm in zip(offs,names):
        e=min(x for x in ends if x>o)
        texs.append((nm,b[o:e],o))
    return texs

def tim3_info(t):
    sig=t[:4]; pics=u16(t,6)
    p=16
    total,clut,img=struct.unpack_from('<3I',t,p); hs,cc=struct.unpack_from('<2H',t,p+12)
    pf,mm,ct,it=t[p+16:p+20]; w,h=struct.unpack_from('<2H',t,p+20)
    tex0=struct.unpack_from('<Q',t,p+24)[0]
    return dict(sig=sig,pics=pics,total=total,clut=clut,img=img,hs=hs,cc=cc,clut_type=ct,img_type=it,w=w,h=h,tex0=tex0)

# ---------------------------------------------------------------- writers
def _pad16(buf):
    while len(buf) % 16: buf.append(0)

def vb_write(vb):
    """vb: dict(pos=[(x,y,z)], flags=[int], nrm=[(b,b,b)] or None, uv=[(h,h)], col=[(r,g,b,a)] or None,
    wt=[((0,j1,j2,j3),(w1,w2,w3,w4))] or None, mat=int, extra=bytes(8))"""
    n=len(vb['pos'])
    out=bytearray(32)
    po=len(out)
    for (x,y,z),fl in zip(vb['pos'],vb['flags']): out+=struct.pack('<3fi',x,y,z,fl)
    _pad16(out)
    no=0
    if vb.get('nrm') is not None:
        no=len(out)
        for t in vb['nrm']: out+=struct.pack('<3bb',t[0],t[1],t[2],0)
        _pad16(out)
    uo=len(out)
    for u,v in vb['uv']: out+=struct.pack('<2h',u,v)
    _pad16(out)
    co=0
    if vb.get('col') is not None:
        co=len(out)
        for c in vb['col']: out+=bytes(c)
        _pad16(out)
    wo=0
    if vb.get('wt') is not None:
        wo=len(out)
        for j,w in vb['wt']: out+=bytes(j)+bytes(w)
        _pad16(out)
    struct.pack_into('<5IHH',out,0,po,no,uo,co,wo,n,vb['mat'])
    out[24:32]=vb.get('extra',bytes(8))
    return bytes(out)

def mdb_write(m, bone_table):
    """m: dict(vbs=[...], hdr=bytes(20)); bone_table: raw bytes (16 per bone)."""
    nv=len(m['vbs']); nb=len(bone_table)//16
    head=bytearray(b'mdb\0'+bytes(4)+struct.pack('<HH',nb,nv)+m['hdr'])
    tab_at=len(head); head+=bytes(4*nv); _pad16(head)
    bone_at=len(head); head+=bone_table; _pad16(head)
    for i,vb in enumerate(m['vbs']):
        struct.pack_into('<I',head,tab_at+4*i,len(head))
        head+=vb_write(vb); _pad16(head)
    struct.pack_into('<I',head,4,bone_at)
    return bytes(head)

def scr_write(kind, nodes, bone_table, anchor):
    """nodes: [dict(name, mdb=dict, srt=9 floats, tail=bytes(60))]; anchor: constant node_off+unk4 of the original."""
    n=len(nodes)
    out=bytearray(b'scr\0'+struct.pack('<III',kind,n,0))
    tab=len(out); out+=bytes(4*n); _pad16(out)
    mdb_offs=[]
    for nd in nodes:
        mdb_offs.append(len(out)); out+=mdb_write(nd['mdb'],bone_table); _pad16(out)
    for i,nd in enumerate(nodes):
        no=len(out); struct.pack_into('<I',out,tab+4*i,no)
        e=bytearray(112)
        struct.pack_into('<iI',e,0,mdb_offs[i]-no,(anchor-no)&0xffffffff)
        e[8:16]=nd['name'].encode('latin1')[:8].ljust(8,b'\0')
        struct.pack_into('<9f',e,16,*nd['srt'])
        e[52:112]=nd['tail']
        out+=e
    return bytes(out)

def dat_build(entries, align=0x20):
    n=len(entries)
    out=bytearray(struct.pack('<I',n)+bytes(4*n)+b''.join(t.encode('latin1')[:4].ljust(4,b'\0') for t,_ in entries))
    for i,(t,d) in enumerate(entries):
        a=align
        while len(out)%a: out.append(0)
        if d is None: continue
        struct.pack_into('<I',out,4+4*i,len(out)); out+=d
    while len(out)%align: out.append(0)
    return bytes(out)

def tm3_write(texs):
    """texs: [(name, tim3 bytes)]"""
    n=len(texs)
    out=bytearray(b'TM3\0'+struct.pack('<II',n,4)+bytes(4)+bytes(4*n)+(bytes(4) if n&1 else b''))
    for nm,_ in texs: out+=nm.encode('latin1')[:8].ljust(8,b'\0')
    for i,(nm,t) in enumerate(texs):
        while len(out)%64: out.append(0)
        struct.pack_into('<I',out,16+4*i,len(out)); out+=t
    while len(out)%64: out.append(0)
    return bytes(out)


# ---------------------------------------------------------------- character merge

KEEP=('GH_00','GHA_00','GHA_01','RH_00','RH_01','RH_02','RH_03')

def empty_mdb(bone, mat=0):
    vb=dict(pos=[(0.0,0.0,0.0)]*3, flags=[0x8000,0x8000,0], nrm=[(0,127,0)]*3, uv=[(0,0)]*3,
            col=[(128,128,128,128)]*3, wt=[((0,bone*4,0,0),(100,0,0,0))]*3, mat=mat, extra=bytes(8))
    return dict(vbs=[vb], hdr=bytes(16)+struct.pack('<f',1.0))

def merge_model(game_dat, mod_scr, mod_tm3, keep=KEEP, append_textures=True):
    """The game's file with the mod's nodes (by name; 'keep' nodes stay the game's, any other node is drawn as nothing)
    and textures (by name; new names are appended unless append_textures is False)."""
    ents=dat_read(game_dat)
    out=[]
    mod_kind, mod_nodes = scr_read(mod_scr)
    mods={n['name']: mdb_read(mod_scr, n['mdb_off']) for n in mod_nodes}
    mod_tex=[(nm,t) for nm,t,_ in tm3_read(mod_tm3)]
    for t,d,o in ents:
        if t=='MD' and d is not None and d[:3]==b'scr':
            kind,nodes=scr_read(d)
            anchor=nodes[0]['node_off']+nodes[0]['unk4']
            m0=mdb_read(d,nodes[0]['mdb_off']); bones=d[m0['bone_off']:m0['bone_off']+16*len(m0['bones'])]
            new=[]
            for nd in nodes:
                if nd['name'] in mods: mdb=mods[nd['name']]
                elif nd['name'] in keep: mdb=mdb_read(d,nd['mdb_off'])
                else: mdb=empty_mdb(4 if len(m0['bones'])>4 else 0)
                new.append(dict(name=nd['name'], mdb=dict(vbs=mdb['vbs'], hdr=mdb['hdr']), srt=nd['srt'], tail=nd['tail']))
            d=scr_write(kind,new,bones,anchor)
        elif t=='TM3' and d is not None and mod_tex:
            texs=[(nm,tb) for nm,tb,_ in tm3_read(d)]
            names=[nm for nm,_ in texs]
            for name,tb in mod_tex:
                if name in names: texs[names.index(name)]=(name,tb)
                elif append_textures: texs.append((name,tb)); names.append(name)
            d=tm3_write(texs)
        out.append((t,d))
    return dat_build(out)

def empty_model(game_dat):
    """Every node of every MD drawn as nothing (e.g. Gene's coat tails, pl01)."""
    ents=dat_read(game_dat); out=[]
    for t,d,o in ents:
        if t=='MD' and d is not None and d[:3]==b'scr':
            kind,nodes=scr_read(d)
            anchor=nodes[0]['node_off']+nodes[0]['unk4']
            m0=mdb_read(d,nodes[0]['mdb_off']); bones=d[m0['bone_off']:m0['bone_off']+16*len(m0['bones'])]
            d=scr_write(kind,[dict(name=n['name'],mdb=empty_mdb(0),srt=n['srt'],tail=n['tail']) for n in nodes],bones,anchor)
        out.append((t,d))
    return dat_build(out)

