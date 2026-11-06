#!/usr/bin/env python3
"""convert_drifto_assets.py -- fetch JohnnyHowe/drifto-old and convert its art
into the runtime formats gfx/ loads (Python 3 stdlib only; numpy is used to
speed up PNG flipping if it happens to be installed).

    python3 tools/convert_drifto_assets.py [--out assets] [--build build]
                                           [--source DIR] [--offline]

The reference repo has no license and its car models / skybox come from
Sketchfab / the Unity Asset Store, so neither the originals nor the converted
files are committed: `make assets` fetches the tarball into build/ and
regenerates assets/ (both gitignored).

Output layout (all paths relative to --out):
    models/<name>.dmesh        binary mesh, see DMESH FORMAT below
    textures/<name>.png        textures referenced by .dmesh submeshes
    skybox/<Name>/{px,nx,py,ny,pz,nz}.png
                               GL cubemap faces (GL_TEXTURE_CUBE_MAP_POSITIVE_X
                               ... NEGATIVE_Z), already re-oriented for our
                               right-handed world: sample the cubemap with the
                               plain world-space view direction.
    skybox/<Name>/sky.txt      key=value hints (horizon colour for fog, ...)
    models/<car>.txt           wheel/attachment info for cars (key=value)
    CREDITS.txt                attribution for the fetched assets

DMESH FORMAT (little endian, version 1)
    char     magic[4] = "DMSH"
    u32      version  = 1
    u32      vertexCount, indexCount, submeshCount
    f32      boundsMin[3], boundsMax[3]
    submesh[submeshCount]:
        u32  firstIndex, indexCount
        f32  color[4]      albedo rgba (multiplied with vertex colour/texture)
        f32  emission[3]   added unlit (head lights etc.)
        u32  flags         bit0 = double sided, bit1 = no outline,
                           bit2 = glossy (toon specular highlight),
                           bit3 = alpha test (discard texels with a < 0.5),
                           bit4 = body paint (cars: replaced by the player's colour)
        char texture[64]   file under textures/ ("" = none), NUL padded
    vertex[vertexCount]:   f32 pos[3], normal[3], color[3], uv[2]  (44 bytes)
    u32      indices[indexCount]   (triangles, CCW front faces)

Conventions of the output: right-handed, +Y up, metres, models face -Z with
+X to their right, origin at ground centre for cars.
"""

import argparse
import io
import math
import os
import re
import shutil
import struct
import sys
import tarfile
import urllib.request
import xml.etree.ElementTree as ET
import zlib

try:
    import numpy as _np  # optional, only speeds up skybox PNG flipping
except Exception:  # pragma: no cover
    _np = None

TARBALL_URL = "https://codeload.github.com/JohnnyHowe/drifto-old/tar.gz/HEAD"
FORMAT_VERSION = 1  # bump to force regeneration when the converter changes
CONVERTER_STAMP = "dmesh-v%d-conv11" % FORMAT_VERSION
SKY_STAMP = "skybox-v1"  # separate so mesh-only changes skip the slow PNG work

FLAG_DOUBLE_SIDED = 1
FLAG_NO_OUTLINE = 2
FLAG_GLOSSY = 4
FLAG_ALPHA_TEST = 8
FLAG_PAINT = 16  # car body paint: the renderer swaps in the player's colour


def log(*a):
    print("[assets]", *a, flush=True)


# =============================================================================
# Fetch
# =============================================================================

def fetch_source(build_dir, offline):
    """Download + extract the reference repo into build/drifto-old; returns the
    extracted repo root (the dir containing Assets/)."""
    os.makedirs(build_dir, exist_ok=True)
    tar_path = os.path.join(build_dir, "drifto-old.tar.gz")
    extract_dir = os.path.join(build_dir, "drifto-old")
    root = _find_assets_root(extract_dir)
    if root:
        return root
    if not os.path.exists(tar_path):
        if offline:
            sys.exit("[assets] offline and %s missing" % tar_path)
        log("downloading", TARBALL_URL)
        tmp = tar_path + ".part"
        with urllib.request.urlopen(TARBALL_URL, timeout=120) as r, open(tmp, "wb") as f:
            shutil.copyfileobj(r, f)
        os.replace(tmp, tar_path)
    log("extracting", tar_path)
    with tarfile.open(tar_path, "r:gz") as tf:
        members = [m for m in tf.getmembers() if "/Assets/" in m.name or m.name.endswith("Credits.txt")]
        try:
            tf.extractall(extract_dir, members=members, filter="data")
        except TypeError:  # Python < 3.12
            tf.extractall(extract_dir, members=members)
    root = _find_assets_root(extract_dir)
    if not root:
        sys.exit("[assets] no Assets/ directory in tarball")
    return root


def _find_assets_root(d):
    if not os.path.isdir(d):
        return None
    if os.path.isdir(os.path.join(d, "Assets")):
        return d
    for e in sorted(os.listdir(d)):
        p = os.path.join(d, e)
        if os.path.isdir(os.path.join(p, "Assets")):
            return p
    return None


# =============================================================================
# Minimal Unity YAML parser (the subset Unity actually writes)
# =============================================================================

_DOC_RE = re.compile(r"^--- !u!(\d+) &(-?\d+)(?: stripped)?\s*$", re.M)
_KEY_RE = re.compile(r"^([^\s{\[\"'-][^:]*|-?\d+):(?:\s+(.*))?$")


def _parse_flow(s, i=0):
    """Parse a YAML flow value ({...}, [...], or scalar) starting at s[i]."""
    while i < len(s) and s[i] == " ":
        i += 1
    if i < len(s) and s[i] == "{":
        d = {}
        i += 1
        while True:
            while i < len(s) and s[i] in " ,":
                i += 1
            if i >= len(s) or s[i] == "}":
                return d, i + 1
            j = s.index(":", i)
            key = s[i:j].strip()
            val, i = _parse_flow(s, j + 1)
            d[key] = val
    if i < len(s) and s[i] == "[":
        lst = []
        i += 1
        while True:
            while i < len(s) and s[i] in " ,":
                i += 1
            if i >= len(s) or s[i] == "]":
                return lst, i + 1
            val, i = _parse_flow(s, i)
            lst.append(val)
    # scalar up to , } ] (in flow context)
    j = i
    if j < len(s) and s[j] in "\"'":
        q = s[j]
        k = s.index(q, j + 1)
        return s[j + 1:k], k + 1
    while j < len(s) and s[j] not in ",}]":
        j += 1
    return s[i:j].strip(), j


def _scalar(rest):
    rest = rest.strip()
    if rest.startswith("{") or rest.startswith("["):
        return _parse_flow(rest)[0]
    if len(rest) >= 2 and rest[0] == rest[-1] and rest[0] in "\"'":
        return rest[1:-1]
    return rest


class _Lines:
    def __init__(self, text):
        self.l = []
        for raw in text.split("\n"):
            if not raw.strip():
                continue
            s = raw.lstrip(" ")
            self.l.append([len(raw) - len(s), s.rstrip()])


def _parse_node(L, pos, indent):
    if L.l[pos][1].startswith("- ") or L.l[pos][1] == "-":
        return _parse_seq(L, pos, indent)
    return _parse_map(L, pos, indent)


def _parse_map(L, pos, indent):
    out = {}
    n = len(L.l)
    while pos < n and L.l[pos][0] == indent and not (L.l[pos][1].startswith("- ") or L.l[pos][1] == "-"):
        text = L.l[pos][1]
        m = _KEY_RE.match(text)
        if not m:  # stray continuation; skip
            pos += 1
            continue
        key, rest = m.group(1).strip(), m.group(2)
        pos += 1
        if rest is None or rest == "":
            if pos < n and (L.l[pos][0] > indent or (L.l[pos][0] == indent and (L.l[pos][1].startswith("- ") or L.l[pos][1] == "-"))):
                val, pos = _parse_node(L, pos, L.l[pos][0])
            else:
                val = ""
        else:
            if rest[0] not in "{[\"'":
                # plain scalar possibly continued on deeper-indented lines
                while pos < n and L.l[pos][0] > indent:
                    rest += " " + L.l[pos][1]
                    pos += 1
            val = _scalar(rest)
        out[key] = val
    return out, pos


def _parse_seq(L, pos, indent):
    out = []
    n = len(L.l)
    while pos < n and L.l[pos][0] == indent and (L.l[pos][1].startswith("- ") or L.l[pos][1] == "-"):
        content = L.l[pos][1][2:] if L.l[pos][1] != "-" else ""
        if content and content[0] not in "{[\"'" and _KEY_RE.match(content):
            L.l[pos] = [indent + 2, content]
            val, pos = _parse_map(L, pos, indent + 2)
        elif content == "":
            pos += 1
            if pos < n and L.l[pos][0] > indent:
                val, pos = _parse_node(L, pos, L.l[pos][0])
            else:
                val = ""
        else:
            val = _scalar(content)
            pos += 1
        out.append(val)
    return out, pos


def parse_unity_yaml(text):
    """Returns {fileID(str): (classID(int), typeName, body dict)}."""
    docs = {}
    ms = list(_DOC_RE.finditer(text))
    for k, m in enumerate(ms):
        end = ms[k + 1].start() if k + 1 < len(ms) else len(text)
        body = text[m.end():end]
        L = _Lines(body)
        if not L.l:
            continue
        top, _ = _parse_map(L, 0, L.l[0][0])
        (tname, val), = list(top.items())[:1] or [("", {})]
        docs[m.group(2)] = (int(m.group(1)), tname, val if isinstance(val, dict) else {})
    return docs


def read_text(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def fnum(v, d=0.0):
    try:
        return float(v)
    except (TypeError, ValueError):
        return d


# =============================================================================
# Small vector / matrix helpers (row-major 4x4 lists, column vectors)
# =============================================================================

def mat_identity():
    return [[1.0 if r == c else 0.0 for c in range(4)] for r in range(4)]


def mat_mul(a, b):
    return [[sum(a[r][k] * b[k][c] for k in range(4)) for c in range(4)] for r in range(4)]


def mat_trs(t, q, s):
    x, y, z, w = q
    n = math.sqrt(x * x + y * y + z * z + w * w) or 1.0
    x, y, z, w = x / n, y / n, z / n, w / n
    r = [[1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)],
         [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)],
         [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)]]
    m = mat_identity()
    for i in range(3):
        for j in range(3):
            m[i][j] = r[i][j] * s[j]
        m[i][3] = t[i]
    return m


def mat_point(m, p):
    return (m[0][0] * p[0] + m[0][1] * p[1] + m[0][2] * p[2] + m[0][3],
            m[1][0] * p[0] + m[1][1] * p[1] + m[1][2] * p[2] + m[1][3],
            m[2][0] * p[0] + m[2][1] * p[1] + m[2][2] * p[2] + m[2][3])


def mat3_inv_t(m):
    """Inverse-transpose of the upper 3x3 (for normals) and its determinant."""
    a = [[m[r][c] for c in range(3)] for r in range(3)]
    det = (a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1])
           - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
           + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]))
    if abs(det) < 1e-20:
        return [[1, 0, 0], [0, 1, 0], [0, 0, 1]], 1.0
    cof = [[0.0] * 3 for _ in range(3)]
    for r in range(3):
        for c in range(3):
            r1, r2 = [i for i in range(3) if i != r]
            c1, c2 = [i for i in range(3) if i != c]
            cof[r][c] = ((-1) ** (r + c)) * (a[r1][c1] * a[r2][c2] - a[r1][c2] * a[r2][c1])
    # inverse = adj / det = cof^T / det ; inverse-transpose = cof / det
    return [[cof[r][c] / det for c in range(3)] for r in range(3)], det


def v_norm(v):
    l = math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])
    return (v[0] / l, v[1] / l, v[2] / l) if l > 1e-12 else (0.0, 1.0, 0.0)


def v_sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def v_cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


# =============================================================================
# Geometry container
# =============================================================================

class Material:
    def __init__(self, name, color=(1, 1, 1, 1), emission=(0, 0, 0), texture="", flags=0):
        self.name = name
        self.color = tuple(color)
        self.emission = tuple(emission)
        self.texture = texture
        self.flags = flags

    def key(self):
        return (tuple(round(c, 4) for c in self.color), tuple(round(c, 4) for c in self.emission),
                self.texture, self.flags)


class Model:
    """Positions/normals/colors/uvs + per-material triangle lists."""

    def __init__(self):
        self.pos, self.nrm, self.col, self.uv = [], [], [], []
        self.groups = {}  # material key -> (Material, [indices])

    def add(self, mat, pos, nrm, col, uv, tris):
        """Material rgb is baked into the vertex colours (the shader multiplies
        vertex colour x material colour anyway), so parts that only differed
        by colour share one submesh / draw call."""
        base = len(self.pos)
        cr, cg, cb = mat.color[:3]
        if (cr, cg, cb) != (1, 1, 1):
            col = [(c[0] * cr, c[1] * cg, c[2] * cb) for c in col]
            mat = Material(mat.name, (1.0, 1.0, 1.0, mat.color[3]), mat.emission, mat.texture, mat.flags)
        self.pos += pos
        self.nrm += nrm
        self.col += col
        self.uv += uv
        k = mat.key()
        if k not in self.groups:
            self.groups[k] = (mat, [])
        self.groups[k][1].extend(i + base for i in tris)

    def bounds(self):
        if not self.pos:
            return (0, 0, 0), (0, 0, 0)
        xs, ys, zs = zip(*self.pos)
        return (min(xs), min(ys), min(zs)), (max(xs), max(ys), max(zs))

    def used_bounds(self):
        used = set()
        for _, idx in self.groups.values():
            used.update(idx)
        pts = [self.pos[i] for i in used]
        xs, ys, zs = zip(*pts)
        return (min(xs), min(ys), min(zs)), (max(xs), max(ys), max(zs))

    def transform(self, fn_pos, fn_nrm=None):
        self.pos = [fn_pos(p) for p in self.pos]
        if fn_nrm:
            self.nrm = [fn_nrm(n) for n in self.nrm]

    def compact(self):
        """Drop unreferenced vertices."""
        used = sorted(set(i for _, idx in self.groups.values() for i in idx))
        remap = {o: n for n, o in enumerate(used)}
        self.pos = [self.pos[i] for i in used]
        self.nrm = [self.nrm[i] for i in used]
        self.col = [self.col[i] for i in used]
        self.uv = [self.uv[i] for i in used]
        for k, (m, idx) in list(self.groups.items()):
            self.groups[k] = (m, [remap[i] for i in idx])

    def write(self, path):
        self.compact()
        bmin, bmax = self.bounds()
        groups = [g for g in self.groups.values() if g[1]]
        # Order: outlined opaque parts, then opaque no-outline parts, then
        # alpha-tested / translucent ones. The renderer draws outlines and
        # shadows for a whole model with one call over a prefix of the index
        # buffer, so this order matters.
        def rank(g):
            m = g[0]
            if m.flags & FLAG_ALPHA_TEST or m.color[3] < 0.999:
                return 2
            if m.flags & (FLAG_NO_OUTLINE | FLAG_DOUBLE_SIDED):
                return 1
            return 0
        groups.sort(key=rank)
        indices = []
        subs = []
        for m, idx in groups:
            subs.append((len(indices), len(idx), m))
            indices += idx
        out = io.BytesIO()
        out.write(b"DMSH")
        out.write(struct.pack("<4I", FORMAT_VERSION, len(self.pos), len(indices), len(subs)))
        out.write(struct.pack("<6f", *bmin, *bmax))
        for first, cnt, m in subs:
            tex = m.texture.encode("utf-8")[:63]
            out.write(struct.pack("<2I4f3fI", first, cnt, *m.color, *m.emission, m.flags))
            out.write(tex + b"\0" * (64 - len(tex)))
        vb = []
        for p, n, c, t in zip(self.pos, self.nrm, self.col, self.uv):
            vb.extend((p[0], p[1], p[2], n[0], n[1], n[2], c[0], c[1], c[2], t[0], t[1]))
        out.write(struct.pack("<%df" % len(vb), *vb))
        out.write(struct.pack("<%dI" % len(indices), *indices))
        with open(path, "wb") as f:
            f.write(out.getvalue())
        return len(self.pos), len(indices) // 3, bmin, bmax


# =============================================================================
# Unity project access
# =============================================================================

class UnityProject:
    def __init__(self, root):
        self.root = root
        self.guid_to_path = {}
        for dp, _, files in os.walk(os.path.join(root, "Assets")):
            for fn in files:
                if fn.endswith(".meta"):
                    p = os.path.join(dp, fn)
                    try:
                        with open(p, "r", encoding="utf-8", errors="replace") as f:
                            for line in f:
                                if line.startswith("guid:"):
                                    self.guid_to_path[line.split()[1]] = p[:-5]
                                    break
                    except OSError:
                        pass
        self._mesh_cache = {}
        self._mat_cache = {}

    def path(self, guid):
        return self.guid_to_path.get(guid)

    # --- meshes ---------------------------------------------------------
    def mesh(self, guid):
        if guid in self._mesh_cache:
            return self._mesh_cache[guid]
        p = self.path(guid)
        mesh = None
        if not p or not p.endswith(".asset"):  # builtin (cube/quad) or model file
            self._mesh_cache[guid] = None
            return None
        docs = parse_unity_yaml(read_text(p))
        for cls, tname, body in docs.values():
            if cls == 43:
                mesh = decode_unity_mesh(body)
                break
        self._mesh_cache[guid] = mesh
        return mesh

    # --- materials ------------------------------------------------------
    def material(self, guid, tex_prefix, textures_out):
        key = (guid, tex_prefix)
        if key in self._mat_cache:
            return self._mat_cache[key]
        p = self.path(guid)
        if not p:
            m = Material("missing", (1, 0, 1, 1))
            self._mat_cache[key] = m
            return m
        docs = parse_unity_yaml(read_text(p))
        body = next(b for c, t, b in docs.values() if c == 21)
        props = body.get("m_SavedProperties", {})
        colors = {}
        for e in props.get("m_Colors", []) or []:
            for k, v in e.items():
                colors[k] = (fnum(v.get("r")), fnum(v.get("g")), fnum(v.get("b")), fnum(v.get("a"), 1))
        floats = {}
        for e in props.get("m_Floats", []) or []:
            for k, v in e.items():
                floats[k] = fnum(v)
        texs = {}
        for e in props.get("m_TexEnvs", []) or []:
            for k, v in e.items():
                t = v.get("m_Texture", {})
                if isinstance(t, dict) and t.get("guid"):
                    texs[k] = t["guid"]
        col = colors.get("_Color", (1, 1, 1, 1))
        emi = colors.get("_EmissionColor", (0, 0, 0, 1))
        emissive_on = "_EMISSION" in str(body.get("m_ShaderKeywords", ""))
        tex = ""
        if "_MainTex" in texs and self.path(texs["_MainTex"]):
            src = self.path(texs["_MainTex"])
            base = os.path.splitext(os.path.basename(src))[0]
            tex = tex_prefix + sanitize(base) + os.path.splitext(src)[1].lower()
            textures_out[tex] = src
        # Sketchfab's importer puts the flat colour of untextured materials
        # into _EmissionColor and leaves _Color black (the RX7 does this).
        if max(col[:3]) < 1e-4 and not tex and max(emi[:3]) > 1e-4:
            col = (emi[0], emi[1], emi[2], col[3])
            emi = (0, 0, 0, 1)
        elif not emissive_on and not (max(emi[:3]) > 0.5):
            emi = (0, 0, 0, 1)
        alpha = col[3]
        if floats.get("_Mode", 0) in (2, 3) and alpha >= 0.999:
            alpha = 1.0
        # car materials: glossy toon highlight; textured decals (number plates,
        # door lettering) are thin quads on the body -> no ink outline.
        flags = FLAG_GLOSSY | (FLAG_NO_OUTLINE if tex else 0)
        if floats.get("_Mode", 0) == 1 or "_ALPHATEST_ON" in str(body.get("m_ShaderKeywords", "")):
            flags |= FLAG_ALPHA_TEST  # cutout decal (door lettering)
        m = Material(body.get("m_Name", "mat"), (col[0], col[1], col[2], alpha), emi[:3], tex, flags)
        self._mat_cache[key] = m
        return m


def sanitize(name):
    return re.sub(r"[^a-z0-9_.]+", "_", name.lower()).strip("_")


_FMT = {0: ("f", 4), 1: ("e", 2), 2: ("B", 1), 3: ("b", 1), 4: ("H", 2), 5: ("h", 2),
        6: ("B", 1), 7: ("b", 1), 8: ("H", 2), 9: ("h", 2), 10: ("I", 4), 11: ("i", 4)}
_NORM = {2: 255.0, 3: 127.0, 4: 65535.0, 5: 32767.0}


def decode_unity_mesh(body):
    """Unity Mesh (serializedVersion 10, uncompressed) -> dict with
    pos/nrm/col/uv lists (Unity space) and submesh triangle lists."""
    if fnum(body.get("m_MeshCompression", 0)) != 0:
        raise ValueError("compressed meshes not supported")
    vd = body["m_VertexData"]
    vcount = int(vd["m_VertexCount"])
    data = bytes.fromhex(vd["_typelessdata"].replace(" ", ""))
    chans = vd["m_Channels"]
    # stream layout: channels packed in order; streams follow each other,
    # each aligned to 16 bytes.
    streams = {}
    for ci, ch in enumerate(chans):
        dim = int(ch["dimension"]) & 0xF
        if dim == 0:
            continue
        s = int(ch["stream"])
        size = _FMT[int(ch["format"])][1] * dim
        streams.setdefault(s, 0)
        streams[s] = max(streams[s], int(ch["offset"]) + size)
    stream_base = {}
    off = 0
    for s in sorted(streams):
        stream_base[s] = off
        off += streams[s] * vcount
        off = (off + 15) & ~15

    def channel(ci):
        if ci >= len(chans):
            return None
        ch = chans[ci]
        dim = int(ch["dimension"]) & 0xF
        if dim == 0:
            return None
        fmt = int(ch["format"])
        code, size = _FMT[fmt]
        s = int(ch["stream"])
        stride = streams[s]
        base = stream_base[s] + int(ch["offset"])
        st = struct.Struct("<%d%s" % (dim, code))
        out = []
        norm = _NORM.get(fmt)
        for v in range(vcount):
            vals = st.unpack_from(data, base + v * stride)
            if norm:
                vals = tuple(x / norm for x in vals)
            out.append(vals)
        return out

    pos = channel(0)
    nrm = channel(1)
    col = channel(3)
    uv = channel(4)
    ib = bytes.fromhex(body.get("m_IndexBuffer", "").replace(" ", ""))
    idx32 = int(body.get("m_IndexFormat", 0)) == 1
    isz = 4 if idx32 else 2
    subs = []
    for sm in body.get("m_SubMeshes", []):
        first = int(sm["firstByte"]) // isz
        cnt = int(sm["indexCount"])
        basev = int(sm.get("baseVertex", 0))
        topo = int(sm.get("topology", 0))
        idx = list(struct.unpack_from("<%d%s" % (cnt, "I" if idx32 else "H"), ib, first * isz))
        idx = [i + basev for i in idx]
        if topo == 2:  # quads -> tris
            t = []
            for q in range(0, len(idx), 4):
                a, b, c, d = idx[q:q + 4]
                t += [a, b, c, a, c, d]
            idx = t
        elif topo != 0:
            idx = []
        subs.append(idx)
    return {"name": body.get("m_Name", ""), "pos": pos, "nrm": nrm, "col": col, "uv": uv, "subs": subs}


class Node:
    def __init__(self):
        self.name = ""
        self.active = True
        self.t = (0, 0, 0)
        self.q = (0, 0, 0, 1)
        self.s = (1, 1, 1)
        self.father = None
        self.children = []
        self.mesh = None  # (fileID, guid)
        self.materials = []  # guids
        self.renderer_enabled = True
        self.world = None


def load_prefab_nodes(path):
    """Returns dict transformFileID -> Node with world matrices (Unity space)."""
    docs = parse_unity_yaml(read_text(path))
    go = {}
    for fid, (cls, tn, b) in docs.items():
        if cls == 1:
            go[fid] = b
    nodes = {}
    go_to_tr = {}
    for fid, (cls, tn, b) in docs.items():
        if cls in (4, 224):  # Transform / RectTransform
            n = Node()
            g = go.get(b.get("m_GameObject", {}).get("fileID"), {})
            n.name = g.get("m_Name", "")
            n.active = str(g.get("m_IsActive", "1")) != "0"
            r = b.get("m_LocalRotation", {})
            p = b.get("m_LocalPosition", {})
            s = b.get("m_LocalScale", {})
            n.q = (fnum(r.get("x")), fnum(r.get("y")), fnum(r.get("z")), fnum(r.get("w"), 1))
            n.t = (fnum(p.get("x")), fnum(p.get("y")), fnum(p.get("z")))
            n.s = (fnum(s.get("x"), 1), fnum(s.get("y"), 1), fnum(s.get("z"), 1))
            n.father = b.get("m_Father", {}).get("fileID", "0")
            nodes[fid] = n
            go_to_tr[b.get("m_GameObject", {}).get("fileID")] = fid
    for fid, (cls, tn, b) in docs.items():
        gid = b.get("m_GameObject", {}).get("fileID") if isinstance(b.get("m_GameObject"), dict) else None
        if gid not in go_to_tr:
            continue
        n = nodes[go_to_tr[gid]]
        if cls == 33:
            m = b.get("m_Mesh", {})
            if m.get("guid"):
                n.mesh = (m.get("fileID"), m.get("guid"))
        elif cls == 23:
            n.renderer_enabled = str(b.get("m_Enabled", "1")) != "0"
            n.materials = [m.get("guid") for m in (b.get("m_Materials") or []) if isinstance(m, dict)]
    for fid, n in nodes.items():
        if n.father in nodes:
            nodes[n.father].children.append(fid)

    def world(fid, depth=0):
        n = nodes[fid]
        if n.world is None:
            local = mat_trs(n.t, n.q, n.s)
            if n.father in nodes and depth < 64:
                parent = world(n.father, depth + 1)
                n.world = mat_mul(parent, local)
                n.active = n.active and nodes[n.father].active
            else:
                n.world = local
        return n.world

    for fid in nodes:
        world(fid)
    return nodes


def path_of(nodes, fid):
    parts = []
    while fid in nodes:
        parts.append(nodes[fid].name)
        fid = nodes[fid].father
    return "/".join(reversed(parts))


def append_unity_part(model, proj, node, mat_override, tex_prefix, textures, extra=None, paint=()):
    """Bake one MeshFilter/MeshRenderer into model, converting Unity (LH, +Z
    fwd) -> GL (RH, -Z fwd) by mirroring Z. extra: optional 4x4 applied after
    the node's world matrix (still Unity space). paint: material names that
    get FLAG_PAINT (kept as their own submesh)."""
    mesh = proj.mesh(node.mesh[1])
    if mesh is None:
        return
    M = node.world if extra is None else mat_mul(extra, node.world)
    NM, det = mat3_inv_t(M)
    for si, tris in enumerate(mesh["subs"]):
        guid = node.materials[min(si, len(node.materials) - 1)] if node.materials else None
        mat = mat_override or (proj.material(guid, tex_prefix, textures) if guid else Material("default"))
        if mat.name in paint:
            mat = Material(mat.name, mat.color, mat.emission, mat.texture, mat.flags | FLAG_PAINT)
        used = sorted(set(tris))
        remap = {o: n for n, o in enumerate(used)}
        pos, nrm, col, uv = [], [], [], []
        for i in used:
            p = mat_point(M, mesh["pos"][i])
            pos.append((p[0], p[1], -p[2]))
            if mesh["nrm"]:
                n0 = mesh["nrm"][i]
                n = (NM[0][0] * n0[0] + NM[0][1] * n0[1] + NM[0][2] * n0[2],
                     NM[1][0] * n0[0] + NM[1][1] * n0[1] + NM[1][2] * n0[2],
                     NM[2][0] * n0[0] + NM[2][1] * n0[1] + NM[2][2] * n0[2])
                n = v_norm(n)
                nrm.append((n[0], n[1], -n[2]))
            else:
                nrm.append((0.0, 0.0, 0.0))
            c = mesh["col"][i] if mesh["col"] else (1, 1, 1)
            col.append((c[0], c[1], c[2]))
            t = mesh["uv"][i] if mesh["uv"] else (0, 0)
            # Unity UV origin is bottom-left like GL; stb loads top row first,
            # the renderer flips images on load so UVs stay as authored.
            uv.append((t[0], t[1]))
        # Z mirror reverses handedness: Unity CW fronts -> reverse to CCW.
        # A negative-determinant world matrix mirrors once more.
        flip = det > 0
        out = []
        for k in range(0, len(tris) - 2, 3):
            a, b, c = remap[tris[k]], remap[tris[k + 1]], remap[tris[k + 2]]
            out += [a, c, b] if flip else [a, b, c]
        if not mesh["nrm"]:
            _flat_normals(pos, nrm, out)
        model.add(mat, pos, nrm, col, uv, out)


def _flat_normals(pos, nrm, tris):
    acc = [[0.0, 0.0, 0.0] for _ in pos]
    for k in range(0, len(tris), 3):
        a, b, c = tris[k:k + 3]
        n = v_cross(v_sub(pos[b], pos[a]), v_sub(pos[c], pos[a]))
        for i in (a, b, c):
            acc[i][0] += n[0]
            acc[i][1] += n[1]
            acc[i][2] += n[2]
    for i, a in enumerate(acc):
        nrm[i] = v_norm(a)


# =============================================================================
# Cars
# =============================================================================

CARS = [
    # name, prefab relative to Assets/, target length (m), wheel mesh regex
    ("ae86", "Import/Toyota_Corolla_AE86_Trueno_tofu_delivery/Toyota_Corolla_AE86_Trueno_tofu_delivery.prefab",
     4.2, r"^Object_(9|1[0-9]|20)_"),
    ("rx7", "Import/Mazda_RX7_Stylised/Mazda_RX7_Stylised.prefab", 4.3, None),
]

# Nose direction of each car in Unity prefab space, found by inspecting the
# meshes (head lights / number plates): the AE86 import has its nose at
# Unity -Z, the RX7 (modelled along X) at Unity -X.
CAR_NOSE_UNITY = {"ae86": (0, 0, -1), "rx7": (-1, 0, 0)}

# Tail-pipe exits in final car space (metres, -Z forward), measured from the
# converted geometry (centre of each pipe's rear rim): the AE86 has one pipe
# on the left, the RX7 twin pipes on the right. Exhaust direction is +Z.
CAR_EXHAUSTS = {
    "ae86": {"points": [(-0.441, 0.315, 2.03)], "radius": 0.037},
    "rx7": {"points": [(0.399, 0.227, 1.96), (0.472, 0.227, 1.96)], "radius": 0.03},
}


# Body-paint materials per car (Unity material m_Name), tagged FLAG_PAINT so
# the renderer can recolour just the paintwork: not the tyres, rims, glass,
# lights, trim, number plates or the door lettering. Found by rendering each
# material in a flat colour. AE86: the white upper body (the black bonnet /
# lower half of the panda livery stays). RX7: the white shell plus the
# body-coloured bumpers, skirts, mirrors and wing (that material also covers
# the rims, which go to the wheel model untagged).
CAR_PAINT = {
    "ae86": {"Material.001_0"},
    "rx7": {"02_-_Default_4", "Material_9_3"},
}

# Cars whose wheels are baked into the body mesh (no per-wheel nodes): cut
# them out geometrically (see split_baked_wheels). The RX7's front wheels are
# also modelled turned, so the straight rear-left wheel is used for all four.
CAR_SPLIT_BAKED_WHEELS = {"rx7"}


def split_baked_wheels(model):
    """Cut the wheels out of a car whose tyres, rims and brakes are part of
    the body mesh. Works in final car space (metres, -Z forward, ground at
    y = 0). Tyres are the connected parts that touch the ground; each one
    defines a cylinder (axle from the principal axes of its footprint, so a
    turned front wheel works too), and every connected part of any material
    lying inside a cylinder moves to that wheel. Returns (body, {label:
    (centre, axle, radius, half_width, Model)}) with labels fl/fr/rl/rr, or
    None if four tyres are not found."""
    # connected parts per material group (vertices welded by position)
    parts = []  # (group key, [triangle index lists])
    for gk, (mat, idx) in model.groups.items():
        parent = {}

        def find(x):
            while parent.setdefault(x, x) != x:
                parent[x] = parent[parent[x]]
                x = parent[x]
            return x
        key = [tuple(round(c, 4) for c in model.pos[i]) for i in range(len(model.pos))]
        for k in range(0, len(idx), 3):
            a, b, c = (find(key[idx[k + j]]) for j in range(3))
            parent[a] = b
            parent[find(c)] = find(b)
        comps = {}
        for k in range(0, len(idx), 3):
            comps.setdefault(find(key[idx[k]]), []).extend(idx[k:k + 3])
        for tri in comps.values():
            parts.append((gk, tri))

    def bbox(tri):
        pts = [model.pos[i] for i in tri]
        return tuple(min(p[a] for p in pts) for a in range(3)), tuple(max(p[a] for p in pts) for a in range(3))

    tyres = []
    for gk, tri in parts:
        b0, b1 = bbox(tri)
        if b0[1] < 0.01 and b1[1] - b0[1] > 0.3:
            tyres.append((b0, b1, tri))
    if len(tyres) != 4:
        return None
    wheels = {}
    for b0, b1, tri in tyres:
        c = tuple((b0[a] + b1[a]) * 0.5 for a in range(3))
        # axle = minor principal axis of the tyre's footprint in x/z
        pts = [model.pos[i] for i in set(tri)]
        mx = sum(p[0] for p in pts) / len(pts)
        mz = sum(p[2] for p in pts) / len(pts)
        sxx = sum((p[0] - mx) ** 2 for p in pts)
        szz = sum((p[2] - mz) ** 2 for p in pts)
        sxz = sum((p[0] - mx) * (p[2] - mz) for p in pts)
        ang = 0.5 * math.atan2(2 * sxz, sxx - szz)  # major axis angle
        axle = (-math.sin(ang), 0.0, math.cos(ang))  # perpendicular to it
        if abs(axle[0]) < abs(axle[2]):  # keep the axle the one closer to X
            axle = (math.cos(ang), 0.0, math.sin(ang))
        radius = (b1[1] - b0[1]) * 0.5
        hw = max(abs((p[0] - c[0]) * axle[0] + (p[2] - c[2]) * axle[2]) for p in pts)
        label = ("f" if c[2] < 0 else "r") + ("l" if c[0] < 0 else "r")
        wheels[label] = (c, axle, radius, hw, Model())
    if sorted(wheels) != ["fl", "fr", "rl", "rr"]:
        return None

    def inside(tri, w):
        c, axle, radius, hw, _ = w
        for i in tri:
            d = (model.pos[i][0] - c[0], model.pos[i][1] - c[1], model.pos[i][2] - c[2])
            along = d[0] * axle[0] + d[1] * axle[1] + d[2] * axle[2]
            radial2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] - along * along
            if abs(along) > hw + 0.02 or radial2 > (radius + 0.02) ** 2:
                return False
        return True

    body = Model()
    body.pos, body.nrm, body.col, body.uv = model.pos, model.nrm, model.col, model.uv
    for gk, tri in parts:
        mat = model.groups[gk][0]
        owner = next((w for w in wheels.values() if inside(tri, w)), None)
        if owner is None:
            body.groups.setdefault(gk, (mat, []))[1].extend(tri)
        else:
            # wheels are never paint (the RX7's rims share the body's white)
            wm = Material(mat.name, mat.color, mat.emission, mat.texture, mat.flags & ~FLAG_PAINT)
            dst = owner[4]
            base = len(dst.pos)
            used = sorted(set(tri))
            remap = {o: base + n for n, o in enumerate(used)}
            dst.pos += [model.pos[i] for i in used]
            dst.nrm += [model.nrm[i] for i in used]
            dst.col += [model.col[i] for i in used]
            dst.uv += [model.uv[i] for i in used]
            k = wm.key()
            if k not in dst.groups:
                dst.groups[k] = (wm, [])
            dst.groups[k][1].extend(remap[i] for i in tri)
    return body, wheels


def is_ground_junk(name):
    """Sketchfab scenes ship with a floor plane under the car; skip it."""
    n = name.lower()
    return any(k in n for k in ("floor", "backdrop", "shadow"))


def convert_car(proj, name, prefab_rel, target_len, wheel_re, out_dir, textures, report):
    prefab = os.path.join(proj.root, "Assets", prefab_rel)
    nodes = load_prefab_nodes(prefab)
    full, body = Model(), Model()
    wheels = {}  # parent transform fileID -> Model
    for fid, n in sorted(nodes.items(), key=lambda kv: path_of(nodes, kv[0])):
        if not n.mesh or not n.active or not n.renderer_enabled:
            continue
        mesh = proj.mesh(n.mesh[1])
        if mesh is None:
            continue
        if is_ground_junk(path_of(nodes, fid) + " " + mesh["name"]):
            report.append("  %s: skipped %s" % (name, mesh["name"]))
            continue
        paint = CAR_PAINT.get(name, ())
        append_unity_part(full, proj, n, None, name + "_", textures, paint=paint)
        if wheel_re and re.search(wheel_re, mesh["name"]):
            w = wheels.setdefault(n.father, Model())
            append_unity_part(w, proj, n, None, name + "_", textures)  # wheels: never paint
        else:
            append_unity_part(body, proj, n, None, name + "_", textures, paint=paint)

    # Unity -> GL already mirrored Z. Now rotate about Y so the nose is -Z,
    # then scale to target length, centre on X/Z, put the tyres on y = 0.
    nose = CAR_NOSE_UNITY.get(name, (0, 0, 1))
    nose_gl = (nose[0], nose[1], -nose[2])
    # rotation taking nose_gl to (0,0,-1) about Y
    ang = math.atan2(nose_gl[0], -nose_gl[2])  # yaw of nose measured from -Z towards +X
    ca, sa = math.cos(ang), math.sin(ang)

    def rot(p):  # rotate by -ang about Y (maps nose_gl -> -Z)
        return (ca * p[0] + sa * p[2], p[1], -sa * p[0] + ca * p[2])
    models = [full, body] + list(wheels.values())
    for m in models:
        m.transform(rot, rot)
    bmin, bmax = full.used_bounds()
    scale = target_len / (bmax[2] - bmin[2])
    cx, cz, fy = (bmin[0] + bmax[0]) * 0.5, (bmin[2] + bmax[2]) * 0.5, bmin[1]

    def place(p):
        return ((p[0] - cx) * scale, (p[1] - fy) * scale, (p[2] - cz) * scale)
    for m in models:
        m.transform(place)

    def emit(model, mname):
        nv, nt, b0, b1 = model.write(os.path.join(out_dir, mname + ".dmesh"))
        report.append("  model %-16s %6d verts %6d tris  bounds (%.2f %.2f %.2f)..(%.2f %.2f %.2f)" %
                      (mname, nv, nt, *b0, *b1))
        return b0, b1
    emit(full, name)
    info = ["# %s: generated by convert_drifto_assets.py" % CONVERTER_STAMP,
            "# car local space: -Z forward, +X right, origin = ground centre, metres",
            "length=%.4f" % target_len]
    if name in CAR_SPLIT_BAKED_WHEELS and not wheels:
        split = split_baked_wheels(body)
        if split is None:
            report.append("  %s: could not find the four baked-in wheels" % name)
        else:
            body, baked = split
            emit(body, name + "_body")
            c, axle, radius, hw, w = baked["rl"]
            w.transform(lambda p: (p[0] - c[0], p[1] - c[1], p[2] - c[2]))
            emit(w, name + "_wheel")
            info.append("# %s_wheel is the rear-left wheel centred on its axle (axle = X axis,"
                        " hub cap facing -X); yaw it by pi for right-hand wheels" % name)
            info.append("wheel_radius=%.4f" % radius)
            info.append("wheel_width=%.4f" % (2 * hw))
            for label in ("fl", "fr", "rl", "rr"):
                info.append("wheel_%s=%.4f %.4f %.4f" % ((label,) + baked[label][0]))
    if wheels:
        emit(body, name + "_body")
        centres = {}
        for fid, w in wheels.items():
            b0, b1 = w.used_bounds()
            c = tuple((b0[i] + b1[i]) * 0.5 for i in range(3))
            label = ("f" if c[2] < 0 else "r") + ("l" if c[0] < 0 else "r")
            centres[label] = (c, b0, b1, w)
        if "fl" in centres:
            c, b0, b1, w = centres["fl"]
            radius = max(b1[1] - b0[1], b1[2] - b0[2]) * 0.5
            w.transform(lambda p: (p[0] - c[0], p[1] - c[1], p[2] - c[2]))
            emit(w, name + "_wheel")
            info.append("# %s_wheel is the front-left wheel centred on its axle (axle = X axis,"
                        " hub cap facing -X); yaw it by pi for right-hand wheels" % name)
            info.append("wheel_radius=%.4f" % radius)
            info.append("wheel_width=%.4f" % (b1[0] - b0[0]))
        for label in ("fl", "fr", "rl", "rr"):
            if label in centres:
                info.append("wheel_%s=%.4f %.4f %.4f" % ((label,) + centres[label][0]))
    ex = CAR_EXHAUSTS.get(name)
    if ex:
        info.append("# tail pipes (nitro flames): exhaust_<i> = pipe exit centre, flames point +Z")
        info.append("exhaust_count=%d" % len(ex["points"]))
        info.append("exhaust_radius=%.4f" % ex["radius"])
        for i, p in enumerate(ex["points"]):
            info.append("exhaust_%d=%.4f %.4f %.4f" % ((i,) + tuple(p)))
    with open(os.path.join(out_dir, name + ".txt"), "w") as f:
        f.write("\n".join(info) + "\n")


# =============================================================================
# Trees (BrokenVector LowPolyTreePack .dae)
# =============================================================================

def png_read(path):
    """Minimal PNG decoder -> (w, h, channels, bytearray rows top-first)."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a png: " + path)
    pos = 8
    idat = []
    w = h = bitdepth = ctype = interlace = 0
    palette = None
    trns = None
    while pos < len(data):
        ln, typ = struct.unpack(">I4s", data[pos:pos + 8])
        chunk = data[pos + 8:pos + 8 + ln]
        pos += 12 + ln
        if typ == b"IHDR":
            w, h, bitdepth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", chunk)
        elif typ == b"PLTE":
            palette = chunk
        elif typ == b"tRNS":
            trns = chunk
        elif typ == b"IDAT":
            idat.append(chunk)
        elif typ == b"IEND":
            break
    if bitdepth != 8 or interlace:
        raise ValueError("unsupported png (bitdepth %d interlace %d): %s" % (bitdepth, interlace, path))
    ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(b"".join(idat))
    out = _png_unfilter(raw, w, h, ch)
    if ctype == 3:
        rgb = bytearray(w * h * 3)
        for i in range(w * h):
            k = out[i] * 3
            rgb[i * 3:i * 3 + 3] = palette[k:k + 3]
        return w, h, 3, rgb
    del trns  # transparency chunk unused (no paletted alpha in the sources)
    return w, h, ch, out


def _png_unfilter(raw, w, h, bpp):
    stride = w * bpp
    if _np is not None:
        return _png_unfilter_np(raw, w, h, bpp)
    out = bytearray(stride * h)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        ft = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        if ft == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 255
        elif ft == 2:
            line = bytearray((a + b) & 255 for a, b in zip(line, prev))
        elif ft == 3:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 255
        elif ft == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return out


def _png_unfilter_np(raw, w, h, bpp):
    np = _np
    stride = w * bpp
    buf = np.frombuffer(raw, dtype=np.uint8).reshape(h, stride + 1)
    ftypes = buf[:, 0]
    data = buf[:, 1:].astype(np.int32)
    out = np.zeros((h, stride), dtype=np.int32)
    prev = np.zeros(stride, dtype=np.int32)
    for y in range(h):
        line = data[y]
        ft = ftypes[y]
        if ft == 0:
            cur = line.copy()
        elif ft == 1:
            # cumulative sum per channel lane
            cur = line.reshape(w, bpp).cumsum(axis=0).reshape(-1) & 255
        elif ft == 2:
            cur = (line + prev) & 255
        else:
            cur = line.copy()
            if ft == 3:
                for i in range(0, stride, bpp):
                    left = cur[i - bpp:i] if i else 0
                    cur[i:i + bpp] = (cur[i:i + bpp] + ((left + prev[i:i + bpp]) >> 1)) & 255
            else:
                cl = cur.tolist()
                pl = prev.tolist()
                for i in range(stride):
                    a = cl[i - bpp] if i >= bpp else 0
                    b = pl[i]
                    c = pl[i - bpp] if i >= bpp else 0
                    pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                    pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                    cl[i] = (cl[i] + pr) & 255
                cur = np.array(cl, dtype=np.int32)
        out[y] = cur
        prev = cur
    return bytearray(out.astype(np.uint8).tobytes())


def png_write(path, w, h, ch, pixels):
    ctype = {1: 0, 2: 4, 3: 2, 4: 6}[ch]
    stride = w * ch
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += pixels[y * stride:(y + 1) * stride]

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, ctype, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 6)))
        f.write(chunk(b"IEND", b""))


def _dae_floats(src_el, ns):
    fa = src_el.find("c:float_array", ns)
    vals = [float(x) for x in fa.text.split()] if fa is not None and fa.text else []
    acc = src_el.find("c:technique_common/c:accessor", ns)
    stride = int(acc.get("stride", "1")) if acc is not None else 1
    return [tuple(vals[i:i + stride]) for i in range(0, len(vals) - stride + 1, stride)]


def load_dae(path):
    """Returns list of (positions, normals, uvs, triangles[(pi,ni,ti)*3])
    per primitive, with the scene node matrices applied."""
    ns = {"c": "http://www.collada.org/2005/11/COLLADASchema"}
    root = ET.parse(path).getroot()
    geoms = {}
    for g in root.iterfind(".//c:library_geometries/c:geometry", ns):
        mesh = g.find("c:mesh", ns)
        if mesh is None:
            continue
        sources = {s.get("id"): _dae_floats(s, ns) for s in mesh.iterfind("c:source", ns)}
        verts_el = mesh.find("c:vertices", ns)
        vmap = {}
        if verts_el is not None:
            for inp in verts_el.iterfind("c:input", ns):
                vmap[inp.get("semantic")] = inp.get("source")[1:]
        prims = []
        for prim in list(mesh.iterfind("c:polylist", ns)) + list(mesh.iterfind("c:triangles", ns)) + list(mesh.iterfind("c:polygons", ns)):
            inputs = {}
            maxoff = 0
            for inp in prim.iterfind("c:input", ns):
                off = int(inp.get("offset"))
                maxoff = max(maxoff, off)
                sem = inp.get("semantic")
                srcid = inp.get("source")[1:]
                if sem == "VERTEX":
                    inputs["POSITION"] = (off, vmap.get("POSITION", srcid))
                    if "NORMAL" in vmap:
                        inputs.setdefault("NORMAL", (off, vmap["NORMAL"]))
                elif sem == "TEXCOORD" and int(inp.get("set", "0")) != 0:
                    continue
                else:
                    inputs[sem] = (off, srcid)
            nin = maxoff + 1
            plist = []
            for pel in prim.iterfind("c:p", ns):
                plist.append([int(x) for x in pel.text.split()])
            vc_el = prim.find("c:vcount", ns)
            polys = []
            if prim.tag.endswith("polygons"):
                for p in plist:
                    polys.append([p[k:k + nin] for k in range(0, len(p), nin)])
            else:
                p = plist[0] if plist else []
                if vc_el is not None and vc_el.text:
                    counts = [int(x) for x in vc_el.text.split()]
                else:
                    counts = [3] * (len(p) // (3 * nin))
                k = 0
                for c in counts:
                    polys.append([p[(k + j) * nin:(k + j + 1) * nin] for j in range(c)])
                    k += c
            tris = []
            for poly in polys:
                for j in range(1, len(poly) - 1):
                    tris.append((poly[0], poly[j], poly[j + 1]))
            prims.append({"inputs": inputs, "sources": sources, "tris": tris, "material": prim.get("material")})
        geoms[g.get("id")] = prims

    def node_matrix(n):
        m = mat_identity()
        for child in n:
            tag = child.tag.split("}")[1]
            v = [float(x) for x in (child.text or "").split()]
            if tag == "matrix":
                t = [[v[r * 4 + c] for c in range(4)] for r in range(4)]
            elif tag == "translate":
                t = mat_identity()
                t[0][3], t[1][3], t[2][3] = v
            elif tag == "scale":
                t = mat_identity()
                t[0][0], t[1][1], t[2][2] = v
            elif tag == "rotate":
                ax, ang = v_norm(v[:3]), math.radians(v[3])
                s = math.sin(ang / 2)
                t = mat_trs((0, 0, 0), (ax[0] * s, ax[1] * s, ax[2] * s, math.cos(ang / 2)), (1, 1, 1))
            else:
                continue
            m = mat_mul(m, t)
        return m

    out = []

    def walk(n, parent):
        M = mat_mul(parent, node_matrix(n))
        for ig in n.iterfind("c:instance_geometry", ns):
            for prim in geoms.get(ig.get("url")[1:], []):
                out.append((M, prim))
        for c in n.iterfind("c:node", ns):
            walk(c, M)

    for vs in root.iterfind(".//c:library_visual_scenes/c:visual_scene", ns):
        for n in vs.iterfind("c:node", ns):
            walk(n, mat_identity())
    up = root.find("c:asset/c:up_axis", ns)
    return out, (up.text.strip() if up is not None else "Y_UP")


TREE_PALETTES = ["Normal", "Fall", "Cold", "Dry"]


def convert_trees(proj, out_dir, report):
    base = os.path.join(proj.root, "Assets", "BrokenVector", "LowPolyTreePack")
    models_dir = os.path.join(base, "Models")
    if not os.path.isdir(models_dir):
        report.append("  trees: LowPolyTreePack missing, skipped")
        return
    sheets = {}
    for pal in TREE_PALETTES:
        p = os.path.join(base, "Textures", "Colorsheet Tree %s.png" % pal)
        if os.path.exists(p):
            sheets[pal] = png_read(p)
    # Unity import scale for the pack (ModelImporter globalScale)
    names = sorted(f for f in os.listdir(models_dir) if f.lower().endswith(".dae"))
    for fn in names:
        meta = os.path.join(models_dir, fn + ".meta")
        gscale = 1.0
        if os.path.exists(meta):
            m = re.search(r"globalScale:\s*([0-9.eE+-]+)", read_text(meta))
            if m:
                gscale = float(m.group(1))
        stem = sanitize(os.path.splitext(fn)[0].replace(" ", "_"))  # tree_type0_01
        parts, up = load_dae(os.path.join(models_dir, fn))
        for pal, sheet in sheets.items():
            model = Model()
            for M, prim in parts:
                # Collada (Z-up, right-handed) -> GL (Y-up, right-handed):
                # (x, y, z) -> (x, z, -y)
                if up == "Z_UP":
                    C = [[1, 0, 0, 0], [0, 0, 1, 0], [0, -1, 0, 0], [0, 0, 0, 1]]
                    W = mat_mul(C, M)
                else:
                    W = M
                NM, det = mat3_inv_t(W)
                srcs = prim["sources"]
                inp = prim["inputs"]
                P = srcs[inp["POSITION"][1]]
                N = srcs[inp["NORMAL"][1]] if "NORMAL" in inp else None
                T = srcs[inp["TEXCOORD"][1]] if "TEXCOORD" in inp else None
                pos, nrm, col, uv, tris = [], [], [], [], []
                for tri in prim["tris"]:
                    # face colour = colorsheet texel at the triangle's UV centroid
                    if T:
                        us = [T[v[inp["TEXCOORD"][0]]] for v in tri]
                        cu = sum(u[0] for u in us) / 3.0
                        cv = sum(u[1] for u in us) / 3.0
                    else:
                        cu = cv = 0.5
                    w, h, ch, px = sheet
                    xi = min(w - 1, max(0, int(cu * w)))
                    yi = min(h - 1, max(0, int((1.0 - cv) * h)))
                    k = (yi * w + xi) * ch
                    c = (px[k] / 255.0, px[k + 1 if ch > 2 else k] / 255.0, px[k + 2 if ch > 2 else k] / 255.0)
                    vids = []
                    for v in tri:
                        p = mat_point(W, P[v[inp["POSITION"][0]]])
                        pos.append((p[0] * gscale, p[1] * gscale, p[2] * gscale))
                        if N:
                            n0 = N[v[inp["NORMAL"][0]]]
                            n = v_norm((NM[0][0] * n0[0] + NM[0][1] * n0[1] + NM[0][2] * n0[2],
                                        NM[1][0] * n0[0] + NM[1][1] * n0[1] + NM[1][2] * n0[2],
                                        NM[2][0] * n0[0] + NM[2][1] * n0[1] + NM[2][2] * n0[2]))
                        else:
                            n = (0.0, 0.0, 0.0)
                        nrm.append(n)
                        col.append(c)
                        uv.append(T[v[inp["TEXCOORD"][0]]][:2] if T else (0.0, 0.0))
                        vids.append(len(pos) - 1)
                    tris += vids if det > 0 else [vids[0], vids[2], vids[1]]
                if not N:
                    _flat_normals(pos, nrm, tris)
                model.add(Material("tree"), pos, nrm, col, uv, tris)
            model = weld(model)
            # trunk base at origin
            bmin, bmax = model.used_bounds()
            cx, cz = 0.0, 0.0
            model.transform(lambda p: (p[0] - cx, p[1] - bmin[1], p[2] - cz))
            name = stem if pal == "Normal" else stem + "_" + pal.lower()
            nv, nt, b0, b1 = model.write(os.path.join(out_dir, name + ".dmesh"))
            if pal == "Normal":
                report.append("  model %-16s %6d verts %6d tris  height %.1f m (+ _fall/_cold/_dry)" %
                              (name, nv, nt, b1[1] - b0[1]))


def weld(model):
    """Merge identical vertices (flat-shaded low-poly keeps face normals)."""
    out = Model()
    for k, (mat, idx) in model.groups.items():
        lut = {}
        pos, nrm, col, uv, tris = [], [], [], [], []
        for i in idx:
            key = (tuple(round(x, 5) for x in model.pos[i]), tuple(round(x, 3) for x in model.nrm[i]),
                   tuple(round(x, 3) for x in model.col[i]))
            j = lut.get(key)
            if j is None:
                j = lut[key] = len(pos)
                pos.append(model.pos[i])
                nrm.append(model.nrm[i])
                col.append(model.col[i])
                uv.append(model.uv[i])
            tris.append(j)
        out.add(mat, pos, nrm, col, uv, tris)
    return out


# =============================================================================
# Skyboxes
# =============================================================================

# Unity's "Skybox/6 Sided" shader: _FrontTex = +Z, _BackTex = -Z,
# _LeftTex = +X, _RightTex = -X, _UpTex = +Y, _DownTex = -Y (Unity space).
# Unity's (left-handed) cube convention is the same as GL's cube map
# convention, so a face image for Unity axis A would be uploaded as-is to
# GL target A *if* we sampled with Unity-space directions. Our world is the
# Z-mirror of Unity's, so the GL face for +Z(GL) is Unity's -Z face, and every
# face image is mirrored: horizontally for the side faces, vertically for
# top/bottom. Doing that here means the renderer samples with the plain world
# direction.
SKY_FACES = [
    # gl face, unity material slot, mirror ('h' or 'v')
    ("px", "_LeftTex", "h"),   # GL +X = Unity +X
    ("nx", "_RightTex", "h"),  # GL -X = Unity -X
    ("py", "_UpTex", "v"),     # GL +Y = Unity +Y
    ("ny", "_DownTex", "v"),   # GL -Y = Unity -Y
    ("pz", "_BackTex", "h"),   # GL +Z = Unity -Z
    ("nz", "_FrontTex", "h"),  # GL -Z = Unity +Z
]


def _mirror(w, h, ch, px, mode):
    stride = w * ch
    if _np is not None:
        a = _np.frombuffer(bytes(px), dtype=_np.uint8).reshape(h, w, ch)
        a = a[:, ::-1] if mode == "h" else a[::-1]
        return bytearray(_np.ascontiguousarray(a).tobytes())
    out = bytearray(len(px))
    if mode == "v":
        for y in range(h):
            out[y * stride:(y + 1) * stride] = px[(h - 1 - y) * stride:(h - y) * stride]
    else:
        for y in range(h):
            row = px[y * stride:(y + 1) * stride]
            pix = [row[i:i + ch] for i in range(0, stride, ch)]
            out[y * stride:(y + 1) * stride] = b"".join(reversed(pix))
    return out


def convert_skyboxes(proj, out_dir, report):
    mats_dir = os.path.join(proj.root, "Assets", "Environment", "Skyboxes", "Materials")
    if not os.path.isdir(mats_dir):
        report.append("  skyboxes: missing")
        return []
    names = []
    for fn in sorted(os.listdir(mats_dir)):
        if not fn.endswith(".mat"):
            continue
        docs = parse_unity_yaml(read_text(os.path.join(mats_dir, fn)))
        body = next(b for c, t, b in docs.values() if c == 21)
        texs = {}
        for e in body.get("m_SavedProperties", {}).get("m_TexEnvs", []) or []:
            for k, v in e.items():
                t = v.get("m_Texture", {})
                if isinstance(t, dict) and t.get("guid"):
                    texs[k] = proj.path(t["guid"])
        if not all(texs.get(s) for _, s, _ in SKY_FACES):
            continue
        name = os.path.splitext(fn)[0]
        dst = os.path.join(out_dir, name)
        os.makedirs(dst, exist_ok=True)
        stamp = os.path.join(dst, "sky.txt")
        if os.path.exists(stamp) and SKY_STAMP in read_text(stamp):
            names.append(name)
            continue
        horizon = [0.0, 0.0, 0.0]
        nh = 0
        for face, slot, mode in SKY_FACES:
            w, h, ch, px = png_read(texs[slot])
            if face in ("px", "nx", "pz", "nz"):
                # average a band just above the horizon line for the fog colour
                y0, y1 = int(h * 0.44), int(h * 0.5)
                for y in range(y0, y1, 4):
                    for x in range(0, w, 16):
                        k = (y * w + x) * ch
                        horizon[0] += px[k]
                        horizon[1] += px[k + 1]
                        horizon[2] += px[k + 2]
                        nh += 1
            png_write(os.path.join(dst, face + ".png"), w, h, ch, _mirror(w, h, ch, px, mode))
        horizon = [c / (255.0 * max(nh, 1)) for c in horizon]
        with open(stamp, "w") as f:
            f.write("# %s\n" % SKY_STAMP)
            f.write("source=%s\n" % fn)
            f.write("horizon=%.4f %.4f %.4f\n" % tuple(horizon))
        names.append(name)
        report.append("  skybox %-16s horizon (%.2f %.2f %.2f)" % (name, *horizon))
    return names


# =============================================================================
# main
# =============================================================================

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", default="assets")
    ap.add_argument("--build", default="build")
    ap.add_argument("--source", help="use an already extracted drifto-old checkout")
    ap.add_argument("--offline", action="store_true")
    ap.add_argument("--force", action="store_true", help="regenerate everything")
    args = ap.parse_args()

    root = args.source if args.source else fetch_source(args.build, args.offline)
    log("source:", root)
    out = args.out
    stamp_path = os.path.join(out, ".stamp")
    if args.force and os.path.isdir(out):
        shutil.rmtree(out)
    models_dir = os.path.join(out, "models")
    tex_dir = os.path.join(out, "textures")
    sky_dir = os.path.join(out, "skybox")
    for d in (models_dir, tex_dir, sky_dir):
        os.makedirs(d, exist_ok=True)

    proj = UnityProject(root)
    report = []
    textures = {}
    for name, prefab, length, wheel_re in CARS:
        if os.path.exists(os.path.join(root, "Assets", prefab)):
            convert_car(proj, name, prefab, length, wheel_re, models_dir, textures, report)
        else:
            report.append("  %s: prefab missing" % name)
    for dst, src in textures.items():
        shutil.copyfile(src, os.path.join(tex_dir, dst))
        report.append("  texture %s" % dst)
    smoke = os.path.join(root, "Assets", "Car", "smoke.png")
    if os.path.exists(smoke):  # tyre smoke sprite used by drifto-old
        shutil.copyfile(smoke, os.path.join(tex_dir, "smoke.png"))
        report.append("  texture smoke.png")
    convert_trees(proj, models_dir, report)
    convert_skyboxes(proj, sky_dir, report)

    with open(os.path.join(out, "CREDITS.txt"), "w") as f:
        f.write("Generated by tools/convert_drifto_assets.py from %s\n" % TARBALL_URL)
        f.write("(JohnnyHowe/drifto-old -- no license; do not redistribute.)\n\n")
        for lic in ("Import/Toyota_Corolla_AE86_Trueno_tofu_delivery/license.txt",
                    "Import/Mazda_RX7_Stylised/license.txt"):
            p = os.path.join(root, "Assets", lic)
            if os.path.exists(p):
                f.write(read_text(p).strip() + "\n\n")
        f.write("Low Poly Tree Pack by Broken Vector (Unity Asset Store).\n")
        f.write("Skyboxes: Unity Asset Store skybox pack bundled with drifto-old.\n")
    with open(stamp_path, "w") as f:
        f.write(CONVERTER_STAMP + "\n")
    print("\n".join(report))
    log("done ->", out)


if __name__ == "__main__":
    main()
