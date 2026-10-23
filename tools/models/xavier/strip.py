"""Triangle list -> God Hand strip vertex buffers (flags: 0x8000 = no triangle, 0/1 = triangle winding)."""
from collections import defaultdict

def stripify(tris):
    """tris: list of (a,b,c) unique-vertex ids with the wanted winding. Returns list of strips [ [ids...] ]."""
    edge_tris=defaultdict(list)
    for t,(a,b,c) in enumerate(tris):
        for e in ((a,b),(b,c),(c,a)): edge_tris[frozenset(e)].append(t)
    used=[False]*len(tris)
    def neighbors(t):
        a,b,c=tris[t]; out=[]
        for e in ((a,b),(b,c),(c,a)):
            for u in edge_tris[frozenset(e)]:
                if u!=t and not used[u]: out.append(u)
        return out
    strips=[]
    order=sorted(range(len(tris)), key=lambda t: len(neighbors(t)))
    for start in order:
        if used[start]: continue
        # choose the starting edge giving the longest strip (try 3 rotations)
        best=None
        for r in range(3):
            a,b,c=tris[start][r:]+tris[start][:r]
            seq=[a,b,c]; mark=[start]; used[start]=True
            while True:
                p,q=seq[-2],seq[-1]
                cand=None
                for u in edge_tris[frozenset((p,q))]:
                    if not used[u]:
                        cand=u; break
                if cand is None: break
                x=[v for v in tris[cand] if v!=p and v!=q]
                if len(x)!=1: break
                seq.append(x[0]); used[cand]=True; mark.append(cand)
            for m in mark: used[m]=False
            if best is None or len(mark)>len(best[1]): best=(seq,mark)
        for m in best[1]: used[m]=True
        strips.append(best[0])
    return strips

def winding_flag(tri_set_orient, a, b, c):
    """flag 0 if (a,b,c) has the wanted orientation, 1 if (b,a,c) has it."""
    return 0 if tri_set_orient((a,b,c)) else 1

def build_buffers(tris, max_verts=1000):
    """Returns list of buffers: each [(vid, flag)] with flags 0x8000/0/1, keeping each buffer <= max_verts."""
    want=set()
    for a,b,c in tris: want.add((a,b,c)); want.add((b,c,a)); want.add((c,a,b))
    bufs=[]; cur=[]
    for s in stripify(tris):
        # split long strips into chunks that fit
        i=0
        while i < len(s)-2:
            room=max_verts-len(cur)
            if room < 3:
                bufs.append(cur); cur=[]; room=max_verts
            seg=s[i:i+room]
            out=[(seg[0],0x8000),(seg[1],0x8000)]
            for k in range(2,len(seg)):
                a,b,c=seg[k-2],seg[k-1],seg[k]
                out.append((c, 0 if (a,b,c) in want else 1))
            cur+=out
            i+=len(seg)-2
    if cur: bufs.append(cur)
    return bufs
