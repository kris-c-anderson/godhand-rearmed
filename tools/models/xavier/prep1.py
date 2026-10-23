import os
WORK=os.environ.get('XAVIER_WORK','xavier_work')
# Blender: Xavier -> Gene skeleton (rest pose), right arm removed. Saves <work>/xavier_stage1.blend
import bpy, bmesh, sys, math
import numpy as np
from mathutils import Vector, Matrix
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__))); sys.path.insert(0,os.path.join(os.path.dirname(os.path.abspath(__file__)),'..'))
from ghmodel import *

GENE=os.environ['GENE_PL00']  # the game's pl/pl00.dat (tools/disc_extract.py)
XAV=os.environ['XAVIER_BLEND']  # Xavier_v1_1_1-README.blend

# Gene joints, Blender coords (x, -z, y)
b=open(GENE,'rb').read(); md=[d for t,d,o in dat_read(b) if t=='MD'][0]
kind,nodes=scr_read(md); m0=mdb_read(md,nodes[0]['mdb_off'])
GW=[]
for bn in m0['bones']:
    p=bn['parent']; base=GW[p] if p>=0 else np.zeros(3)
    GW.append(base+np.array(bn['t']))
G=[np.array((w[0],-w[2],w[1])) for w in GW]

bpy.ops.wm.open_mainfile(filepath=XAV)
arm=bpy.data.objects['kish_080225']
arm.data.pose_position='REST'
names=['XavierBody','XavierHair','XavierNecklace','XavierShoulderPad','Xaviersnakeeyes','XavierEyeR','XavierEyesL']
# Right arm: Xavier's is cut at ARM_CUT along the upper arm (0 shoulder, 1 elbow); the God Hand arm (Gene's GH_00 /
# GHA_00, trimmed to start inside the cut: character.json) continues from there. The shoulder pad is scaled by
# PAD_RADIAL around the upper arm and PAD_AXIAL along it.
ARM_CUT=0.5; PAD_RADIAL=1.16; PAD_AXIAL=1.05
# The cut's open ring is capped (a disc of fur), so nothing shows through the stump around the God Hand arm.
# Butt: the cheeks scaled towards the pelvis by BUTT_SCALE (1 = as modelled; 0.945 after two rounds in game), weighted by the butt bones, so they
# stay inside the loincloth. Hair: every strip pushed out along its normals by HAIR_LIFT, plus up to HAIR_STAGGER
# more for the outer strips (ordered by distance from the head); both 0 now: lifting showed the strips' transparent
# undersides from the front and did not end the z-fighting (which the port's compressed depth range causes, see the
# status doc). HAIR_SHORTEN raises the hair below the ears towards them at the back of the head only (1 = as
# modelled; 0.9 = the back hair below ear level 10% shorter); HAIR_FLARE pushes the hair below the ears back from the neck by that much (metres),
# scaled by how far below the ears it is. All overridable from the environment for variants.
def _env(n,d): return float(os.environ.get(n,d))
BUTT_SCALE=_env('XAVIER_BUTT_SCALE',0.945)
HAIR_LIFT=_env('XAVIER_HAIR_LIFT',0.0); HAIR_STAGGER=_env('XAVIER_HAIR_STAGGER',0.0)
HAIR_SHORTEN=_env('XAVIER_HAIR_SHORTEN',0.9); HAIR_FLARE=_env('XAVIER_HAIR_FLARE',0.0)
HAIR_EAR_Z=1.62   # ear level, Blender z (game y) after scaling to Gene's height
Rarm=['def_arm_0.R','def_arm_1.R','def_arm_1_bridge.R','def_arm_1_elbow_dc_0.R','def_hand_0.R']+['def_finger_%d_%d'%(i,j) for i in range(5) for j in range(3)]

def bonepos(n):
    bn=arm.data.bones[n]; mw=arm.matrix_world
    return np.array(mw@bn.head_local), np.array(mw@bn.tail_local)

# ---------------- transforms
def frame_from(d):
    d=d/np.linalg.norm(d)
    a=np.array((0,0,1.0)) if abs(d[2])<0.9 else np.array((1.0,0,0))
    u=np.cross(d,a); u/=np.linalg.norm(u); v=np.cross(d,u)
    return np.stack([d,u,v],1)  # columns

def rot_between(a,b):
    a=a/np.linalg.norm(a); b=b/np.linalg.norm(b)
    v=np.cross(a,b); c=float(np.dot(a,b))
    if np.linalg.norm(v)<1e-9: return np.eye(3)
    vx=np.array([[0,-v[2],v[1]],[v[2],0,-v[0]],[-v[1],v[0],0]])
    return np.eye(3)+vx+vx@vx*(1/(1+c))

def seg_xform(xh,xt,gh,gt,kperp):
    dx=xt-xh; dg=gt-gh
    R=rot_between(dx,dg)
    F=frame_from(dx)
    S=F@np.diag([np.linalg.norm(dg)/np.linalg.norm(dx),kperp,kperp])@F.T
    A=R@S
    M=np.eye(4); M[:3,:3]=A; M[:3,3]=gh-A@xh
    return M

def uni_xform(xp,gp,k,R=np.eye(3)):
    M=np.eye(4); M[:3,:3]=R*k; M[:3,3]=gp-(R*k)@xp; return M

body=bpy.data.objects['XavierBody']
bw=body.matrix_world
vz=[ (bw@v.co).z for v in body.data.vertices]
xtop=max(vz)
GENE_TOP=1.837
XHIP=np.array((0,-0.01,0.931)); GHIP=np.array((0,0,1.01))
k=(GENE_TOP-GHIP[2])/(xtop-XHIP[2])
print('XTOP',xtop,'k',k)
T0=uni_xform(XHIP,GHIP,k)

X=lambda n: bonepos(n)
xf={}
# legs
for s,g0 in (('L',22),('R',18)):
    hip=X('def_leg_0.'+s)[0]; knee=(X('def_leg_0.'+s)[1]+X('def_leg_1.'+s)[0])/2
    ank=X('def_foot_0.'+s)[0]
    gh=G[g0]; gk=G[g0+1]; ga=np.array((G[g0+2][0],G[g0+2][1],ank[2]*k))
    xf['thigh'+s]=seg_xform(hip,knee,gh,gk,k)
    xf['shin'+s]=seg_xform(knee,ank,gk,ga,k)
    xf['foot'+s]=uni_xform(ank,ga,k)
# left (snake) arm
sh=X('def_arm_0.L')[0]; el=X('def_snake_0')[0]; st=X('def_snake_0')[1]
gs=G[13]; ge=G[14]; gdir=(G[16]-G[14])/np.linalg.norm(G[16]-G[14])
FORE=float(np.linalg.norm(st-el))*k
xf['uparmL']=seg_xform(sh,el,gs,ge,k)
xf['foreL']=seg_xform(el,st,ge,ge+gdir*FORE,k)
Rf=rot_between(st-el, gdir)
xf['snakehead']=uni_xform(st,ge+gdir*FORE,k,Rf)
# right upper arm (shoulder pad)
shR=X('def_arm_0.R')[0]; elR=X('def_arm_1.R')[0]
xf['uparmR']=seg_xform(shR,elR,G[7],G[8],k)
xf['T0']=T0

def group_xform(n):
    if n.startswith('def_leg_0.'): return 'thigh'+n[-1]
    if n.startswith('def_leg_1'): return 'shin'+n[-1]
    if n.startswith('def_foot_0.') or n.startswith('def_toe_0.'): return 'foot'+n[-1]
    if n in ('def_arm_0.L',): return 'uparmL'
    if n in ('def_arm_1_bridge.L','def_arm_1_elbow_dc_0.L','def_snake_0'): return 'foreL'
    if n.startswith('def_snake'): return 'snakehead'
    if n in ('def_arm_0.R','def_arm_1.R','def_arm_1_bridge.R','def_arm_1_elbow_dc_0.R'): return 'uparmR'
    return 'T0'

# Gene bone for each Xavier deform group
def gene_bone(n, pos=None):
    if n in ('def_pelvis_0','def_loincloth_front','def_loincloth_back') or n.startswith('def_butt'): return 17
    if n.startswith('def_waist') or n in ('def_spine_0','def_spine_1'): return 1
    if n=='def_chest_0': return 2
    if n=='def_neck_0': return 3
    if n=='def_shoulder_0.L': return 11
    if n=='def_shoulder_0.R': return 5
    if n=='def_arm_0.L': return 13
    if n in ('def_arm_1_bridge.L','def_arm_1_elbow_dc_0.L'): return 14
    if n=='def_snake_0': return 'snake0'
    if n.startswith('def_snake'): return 16
    if n=='def_arm_0.R': return 7
    if n in ('def_arm_1.R','def_arm_1_bridge.R','def_arm_1_elbow_dc_0.R'): return 8
    for s,g0 in (('L',22),('R',18)):
        if n=='def_leg_0.'+s: return g0
        if n.startswith('def_leg_1') and n.endswith('.'+s): return g0+1
        if n=='def_foot_0.'+s: return g0+2
        if n=='def_toe_0.'+s: return g0+3
    return 4  # head, face, hair, eyes, mouth

deform=set(bn.name for bn in arm.data.bones if bn.use_deform)
snake_h,snake_t=X('def_snake_0')

out_objs=[]
for name in names:
    ob=bpy.data.objects[name]
    me=ob.data.copy(); mw=ob.matrix_world.copy()
    nv=len(me.vertices)
    co=np.array([tuple(v.co) for v in me.vertices])
    sk=ob.data.shape_keys
    if sk:
        basis=np.array([tuple(x.co) for x in sk.reference_key.data])
        co=basis.copy()
        for kb in sk.key_blocks[1:]:
            if kb.value and not kb.mute:
                co+=kb.value*(np.array([tuple(x.co) for x in kb.data])-np.array([tuple(x.co) for x in kb.relative_key.data]))
    P=np.array([tuple(mw@Vector(c)) for c in co])
    gname={g.index:g.name for g in ob.vertex_groups}
    newP=np.zeros_like(P); gw=[dict() for _ in range(nv)]; rsum=np.zeros(nv)
    cloth=np.zeros(nv); butt=np.zeros(nv)
    for i,v in enumerate(me.vertices):
        ws=[(gname[g.group],g.weight) for g in v.groups if gname.get(g.group) in deform and g.weight>0]
        tot=sum(w for _,w in ws)
        if tot<=0: ws=[('def_head_0',1.0)]; tot=1.0
        acc=np.zeros(3); p4=np.append(P[i],1)
        for n,w in ws:
            w/=tot
            acc+=w*(xf[group_xform(n)]@p4)[:3]
            gb=gene_bone(n)
            if gb=='snake0':
                d=snake_t-snake_h; t=np.clip(np.dot(P[i]-snake_h,d)/np.dot(d,d),0,1)
                t2=np.clip((t-0.3)/0.6,0,1)
                gw[i][14]=gw[i].get(14,0)+w*(1-t2); gw[i][15]=gw[i].get(15,0)+w*t2
            else:
                gw[i][gb]=gw[i].get(gb,0)+w
            if n in Rarm: rsum[i]+=w
            if n in ('def_loincloth_front','def_loincloth_back'): cloth[i]+=w
            if n.startswith('def_butt'): butt[i]+=w
        newP[i]=acc
    new=bpy.data.objects.new('X_'+name, me)
    bpy.context.scene.collection.objects.link(new)
    if new.data.shape_keys: new.shape_key_clear()
    for vi,v in enumerate(me.vertices): v.co=Vector(newP[vi])
    new.vertex_groups.clear()
    for gi in range(32): new.vertex_groups.new(name=str(gi))
    for vi,d in enumerate(gw):
        for gb,w in d.items():
            if w>0.001: new.vertex_groups[gb].add([vi],w,'REPLACE')
    me['rsum']=0
    # place along Xavier's right upper arm (0 shoulder, 1 elbow)
    tR=np.dot(P-shR,elR-shR)/np.dot(elR-shR,elR-shR)
    if name=='XavierShoulderPad':
        # a little bigger, so it sits over the upper arm's end; its strap rides the upper arm (Gene's bone 7)
        ax=(G[8]-G[7])/np.linalg.norm(G[8]-G[7]); c=newP.mean(0)
        for v in me.vertices:
            d=np.array(v.co)-c; a=np.dot(d,ax)
            v.co=Vector(c+ax*a*PAD_AXIAL+(d-ax*a)*PAD_RADIAL)
    at=me.attributes.new('cloth','FLOAT','POINT')
    for i in range(nv): at.data[i].value=float(cloth[i])
    if name=='XavierBody' and BUTT_SCALE!=1.0 and butt.any():
        # the cheeks towards the pelvis bone (Gene's bone 17), by their butt weight
        c=np.array(G[17])
        for i,v in enumerate(me.vertices):
            if butt[i]>0: v.co=Vector(c+(np.array(v.co)-c)*(1.0-(1.0-BUTT_SCALE)*min(1.0,butt[i])))
    if name=='XavierHair':
        bm=bmesh.new(); bm.from_mesh(me); bm.verts.ensure_lookup_table(); bm.faces.ensure_lookup_table(); bm.normal_update()
        head=np.array(G[4])
        # strips: connected faces; ordered by their mean distance from the head bone (inner first)
        seen=set(); strips=[]
        for f in bm.faces:
            if f.index in seen: continue
            st=[f]; comp=[]
            while st:
                g=st.pop()
                if g.index in seen: continue
                seen.add(g.index); comp.append(g)
                for e in g.edges:
                    for h in e.link_faces:
                        if h.index not in seen: st.append(h)
            strips.append(comp)
        strips.sort(key=lambda comp: np.mean([np.linalg.norm(np.array(f.calc_center_median())-head) for f in comp]))
        vn={}  # vertex -> outward direction: the mean normal of its faces (vertices are not shared between strips)
        for rank,comp in enumerate(strips):
            lift=HAIR_LIFT+HAIR_STAGGER*rank/max(1,len(strips)-1)
            vs=set(v for f in comp for v in f.verts)
            for v in vs:
                nrm=np.array(v.normal); l=np.linalg.norm(nrm)
                if l<1e-6: continue
                nrm/=l
                if np.dot(nrm,np.array(v.co)-head)<0: nrm=-nrm  # outward, away from the head
                v.co=Vector(np.array(v.co)+nrm*lift)
        if HAIR_SHORTEN!=1.0 or HAIR_FLARE:
            lowest=HAIR_EAR_Z-min(w.co.z for w in bm.verts)
            for v in bm.verts:
                below=HAIR_EAR_Z-v.co.z
                if below<=0: continue
                back=min(1.0,max(0.0,(v.co.y+0.02)/0.06))  # 1 at the back of the head (y > 0.04), 0 at the sides/front
                v.co.z=HAIR_EAR_Z-below*(1.0-(1.0-HAIR_SHORTEN)*back)
                v.co.y+=HAIR_FLARE*min(1.0,below/lowest)*(1 if v.co.y>0 else 0.3)  # mostly the back of the hair
        bm.to_mesh(me); bm.free()
    # delete right arm & mouth interior
    bm=bmesh.new(); bm.from_mesh(me); bm.verts.ensure_lookup_table()
    kill=[]
    if name=='XavierBody':
        mouth=[i for i,s in enumerate(ob.material_slots) if s.material and s.material.name=='mouth']
        killf=[f for f in bm.faces if f.material_index in mouth]
        bmesh.ops.delete(bm,geom=killf,context='FACES')
        bm.verts.ensure_lookup_table()
        # Xavier keeps his own shoulder and the top of his upper arm (skinned to Gene's arm bones, so the joint bends
        # like his own skin); the rest of the arm goes, and Gene's God Hand arm comes out of the upper arm's end.
        killv=[bm.verts[i] for i in range(len(bm.verts)) if rsum[bm.verts[i].index]>=0.5 and tR[bm.verts[i].index]>ARM_CUT]
        bmesh.ops.delete(bm,geom=killv,context='VERTS')
        loose=[v for v in bm.verts if not v.link_faces]
        bmesh.ops.delete(bm,geom=loose,context='VERTS')
        # cap the cut: the open ring of the stump (the boundary edges out along the arm) closed with a fan of fur,
        # each corner keeping its vertex's texture coordinates and material
        bm.edges.ensure_lookup_table(); uvl=bm.loops.layers.uv.active
        ring=[e for e in bm.edges if e.is_boundary and min(v.co.x for v in e.verts)<-0.2]
        if ring:
            ringv=set(v for e in ring for v in e.verts)
            centre=sum((v.co for v in ringv),Vector())/len(ringv)
            cv=bm.verts.new(centre)
            dl=bm.verts.layers.deform.verify(); cv[dl][7]=1.0  # the centre rides Gene's upper-arm bone, like the ring
            ref={v:v.link_loops[0] for v in ringv}
            mat=ring[0].link_faces[0].material_index
            cv2=bm.verts.new(centre); cv2[dl][7]=1.0
            for e in ring:
                a,b=e.verts
                # wind the cap towards the hand (-x), so it faces out of the stump, plus a second copy facing the
                # shoulder: the ring is not flat, so from some angles part of the cap is seen from behind
                front=(b.co-a.co).cross(cv.co-a.co).x<0
                for f in (bm.faces.new((a,b,cv)) if front else bm.faces.new((b,a,cv)), bm.faces.new((b,a,cv2)) if front else bm.faces.new((a,b,cv2))):
                    f.material_index=mat
                    for l in f.loops:
                        if l.vert in ref: l[uvl].uv=ref[l.vert][uvl].uv.copy()
                        else: l[uvl].uv=sum((ref[v][uvl].uv for v in ringv),Vector((0,0)))/len(ringv)  # the centre: the ring's mean
            cv.normal_update(); cv2.normal_update()
            print('CAP',len(ring),'edges, centre',tuple(round(c,3) for c in centre))
        bm.verts.index_update()
    bm.to_mesh(me); bm.free()
    out_objs.append(new)
    print('OBJ',name,'verts',len(me.vertices),'faces',len(me.polygons))

# drop the original Xavier objects/rig from the scene
for o in list(bpy.data.objects):
    if o not in out_objs: bpy.data.objects.remove(o)
np.save(WORK+'/gene_joints.npy',np.array(G))
bpy.ops.wm.save_as_mainfile(filepath=WORK+'/xavier_stage1.blend')
