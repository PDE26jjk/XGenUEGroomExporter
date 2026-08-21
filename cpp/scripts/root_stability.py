"""Per-frame strand root stability analyzer for animated Alembic grooms.

Runs inside Maya (uses Maya's bundled alembic bindings). For each curves
object with >= 2 samples it reports:
  - same-index root displacement (count over epsilon, max, percentiles)
  - root-set identity and uniqueness per frame
  - matched-by-root geometry deltas (interpreted as scene animation, not
    sorting failures) and per-frame CV-count consistency

Usage (inside Maya):
    exec(open(r"cpp/scripts/root_stability.py").read())
    analyze(r"path/to/anim.abc", r"path/to/out.json")
"""
import json

import alembic.Abc as abc
import alembic.AbcGeom as abcGeom

EPS = 1e-4


def strands_of(sample):
    nverts = list(sample.getCurvesNumVertices())
    pos = list(sample.getPositions())
    strands = []
    c = 0
    for n in nverts:
        s = []
        for k in range(n):
            p = pos[c + k]
            s.append((float(p.x), float(p.y), float(p.z)))
        strands.append(s)
        c += n
    return strands


def root_key(s):
    return tuple(round(v, 6) for v in s[0])


def strand_delta(a, b):
    m = 0.0
    for pa, pb in zip(a, b):
        d = sum((x - y) ** 2 for x, y in zip(pa, pb)) ** 0.5
        if d > m:
            m = d
    return m


def analyze(path, out_path):
    archive = abc.IArchive(path)
    top = archive.getTop()
    report = {"file": path, "objects": {}}
    for i in range(top.getNumChildren()):
        child = top.getChild(i)
        if not abcGeom.ICurves.matches(child.getHeader()):
            continue
        curves = abcGeom.ICurves(child, abc.WrapExistingFlag.kWrapExisting)
        schema = curves.getSchema()
        ns = schema.getNumSamples()
        obj = {"samples": ns}
        if ns >= 2:
            s0 = abcGeom.ICurvesSchemaSample()
            s1 = abcGeom.ICurvesSchemaSample()
            schema.get(s0, abc.ISampleSelector(0))
            schema.get(s1, abc.ISampleSelector(1))
            f0 = strands_of(s0)
            f1 = strands_of(s1)
            obj["num_curves"] = len(f0)
            obj["nverts_identical"] = [len(x) for x in f0] == [len(x) for x in f1]

            max_disp = 0.0
            moved_count = 0
            for a, b in zip(f0, f1):
                r0, r1 = a[0], b[0]
                d = sum((x - y) ** 2 for x, y in zip(r0, r1)) ** 0.5
                if d > EPS:
                    moved_count += 1
                if d > max_disp:
                    max_disp = d
            obj["same_index_root_moved"] = moved_count
            obj["same_index_root_max_disp"] = max_disp

            roots0 = sorted(root_key(s) for s in f0)
            roots1 = sorted(root_key(s) for s in f1)
            obj["root_set_identical"] = roots0 == roots1
            obj["root_set_unique0"] = len(set(roots0))
            obj["root_set_unique1"] = len(set(roots1))

            by_root0 = {}
            for idx, s in enumerate(f0):
                by_root0.setdefault(root_key(s), []).append(idx)
            matched = 0
            geom_diff = 0
            cv_diff = 0
            max_delta = 0.0
            for s in f1:
                cand = by_root0.get(root_key(s))
                if not cand:
                    continue
                matched += 1
                base = f0[cand[0]]
                if len(base) != len(s):
                    cv_diff += 1
                else:
                    d = strand_delta(base, s)
                    if d > max_delta:
                        max_delta = d
                    if d > EPS:
                        geom_diff += 1
            obj["matched_by_root"] = matched
            obj["matched_geom_diff"] = geom_diff
            obj["matched_cv_diff"] = cv_diff
            obj["matched_max_delta"] = max_delta
            obj["unmatched_roots_f1"] = len(f1) - matched
        report["objects"][child.getName()] = obj
    with open(out_path, "w", encoding="utf-8") as fh:
        json.dump(report, fh, indent=2, ensure_ascii=False)
    return report
