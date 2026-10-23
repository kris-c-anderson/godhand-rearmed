import os
WORK=os.environ.get('XAVIER_WORK','xavier_work')
# Blender: stage1 -> decimated, material-grouped meshes + textures, dumped as JSON for encoding.
import bpy, bmesh, sys, json, math, os
import numpy as np
from mathutils import Vector
OUT=WORK
TEX=WORK+'/textures'
bpy.ops.wm.open_mainfile(filepath=OUT+'/xavier_stage1.blend')

def obj(n): return bpy.data.objects['X_'+n]
def decimate(o, ratio):
    if ratio>=1: return
    bpy.context.view_layer.objects.active=o
    m=o.modifiers.new('dec','DECIMATE'); m.ratio=ratio; m.use_collapse_triangulate=True
    bpy.ops.object.modifier_apply(modifier='dec')
def tri(o):
    bm=bmesh.new(); bm.from_mesh(o.data); bmesh.ops.triangulate(bm,faces=bm.faces[:]); bm.to_mesh(o.data); bm.free()

budget={'XavierBody':0.5,'XavierHair':0.55,'XavierNecklace':0.6,'XavierShoulderPad':1.0,'Xaviersnakeeyes':1.0,'XavierEyeR':0.03,'XavierEyesL':0.03}
for n,r in budget.items():
    o=obj(n); tri(o); decimate(o,r); print('DEC',n,len(o.data.polygons))

images={}
def img_array(name):
    if name not in images:
        from PIL import Image
        images[name]=np.asarray(Image.open(os.path.join(TEX,name)).convert('RGBA'))
    return images[name]

matimg={'body':'XavierDiffuse.png','head':'XavierFace.png','hair':'Xavier_HAIR.png','eyeR':'Eye_BLUE.png','eyeL':'Eye_BROWN.png','snakeeye':'Xavier_SNAKEEYE.png'}

# collect triangles: (obj, poly) with material name and loop data
faces=[]  # dict(mat, verts=[(pos, uv, weights)], normals per corner)
for n in budget:
    o=obj(n); me=o.data
    me.calc_loop_triangles() if hasattr(me,'calc_loop_triangles') else None
    uvl=me.uv_layers.active.data
    vg={g.index:int(g.name) for g in o.vertex_groups}
    for p in me.polygons:
        mat=o.material_slots[p.material_index].material.name
        corners=[]
        for li in p.loop_indices:
            l=me.loops[li]; v=me.vertices[l.vertex_index]
            w=sorted([(vg[g.group],g.weight) for g in v.groups if g.weight>0.0005], key=lambda x:-x[1])[:3]
            corners.append(dict(key=(n,l.vertex_index), pos=tuple(v.co), nrm=tuple(v.normal), uv=tuple(uvl[li].uv), w=w))
        faces.append(dict(obj=n,mat=mat,c=corners))
print('faces',len(faces))

# ---- split the body atlas into colour groups by UV island
from collections import defaultdict
body=[f for f in faces if f['mat']=='body']
# islands: union faces sharing a corner with identical (vertex, uv)
parent=list(range(len(body)))
def find(a):
    while parent[a]!=a: parent[a]=parent[parent[a]]; a=parent[a]
    return a
owner={}
for i,f in enumerate(body):
    for c in f['c']:
        k=(c['key'],round(c['uv'][0],4),round(c['uv'][1],4))
        if k in owner: parent[find(i)]=find(owner[k])
        else: owner[k]=i
isl=defaultdict(list)
for i in range(len(body)): isl[find(i)].append(i)
A=img_array(matimg['body']).astype(np.float32); H,W=A.shape[:2]
def sample(uv):
    x=min(W-1,max(0,int(uv[0]%1*W))); y=min(H-1,max(0,int((1-uv[1]%1)*H)))
    return A[y,x,:3]
feat=[]; keys=list(isl)
for k in keys:
    cols=[sample(c['uv']) for i in isl[k] for c in body[i]['c']]
    # also sample face centroids
    for i in isl[k]:
        u=sum(c['uv'][0] for c in body[i]['c'])/3; v=sum(c['uv'][1] for c in body[i]['c'])/3
        cols.append(sample((u,v)))
    cols=np.array(cols); feat.append((np.median(cols,0), len(isl[k])))
print('islands',len(keys))
# simple k-means in RGB on island medians, weighted
K=5
X=np.array([f[0] for f in feat]); Wt=np.array([f[1] for f in feat],float)
rng=np.random.default_rng(1)
cent=X[np.argsort(-Wt)[:1]]
while len(cent)<K:
    d=np.min(((X[:,None,:]-cent[None])**2).sum(-1),1)*Wt
    cent=np.vstack([cent,X[np.argmax(d)]])
for it in range(30):
    lab=np.argmin(((X[:,None,:]-cent[None])**2).sum(-1),1)
    for j in range(K):
        if (lab==j).any(): cent[j]=np.average(X[lab==j],0,Wt[lab==j])
for j in range(K): print('GROUP',j,cent[j].round(),int(Wt[lab==j].sum()),'faces')
fur=int(np.argmax([Wt[lab==j].sum() for j in range(K)]))
rest=[i for i in range(len(keys)) if lab[i]!=fur]
cen=[]
for i in rest:
    uv=np.array([c['uv'] for f in isl[keys[i]] for c in body[f]['c']]); cen.append(uv.mean(0))
cen=np.array(cen); K2=3
c2=cen[np.linspace(0,len(cen)-1,K2).astype(int)]
for it in range(50):
    l2=np.argmin(((cen[:,None,:]-c2[None])**2).sum(-1),1)
    for j in range(K2):
        if (l2==j).any(): c2[j]=cen[l2==j].mean(0)
for i in range(len(keys)):
    for f in isl[keys[i]]: body[f]['mat']='body0'
for i,l in zip(rest,l2):
    for f in isl[keys[i]]: body[f]['mat']='body%d'%(l+1)
json.dump(dict(faces=faces,groups=[c.tolist() for c in cent]),open(OUT+'/xavier_faces.json','w'))
