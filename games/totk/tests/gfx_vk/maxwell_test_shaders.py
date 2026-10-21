#!/usr/bin/env python3
"""Hand-assembled Maxwell (Tegra X1 SM5.x) test shaders for the SPIR-V translation chain (docs/windows-port-plan.md C10).

No game data is needed: the vertex shader passes attribute 0 (position) and attribute 1 (texture coordinates) through,
the fragment shader samples combined texture slot 0 at the interpolated coordinates and writes colour target 0, and the
compute shader writes its global invocation x into storage buffer slot 0 (word index = gl_GlobalInvocationID.x).

  maxwell_test_shaders.py <out dir>                       writes vs.bin / ps.bin / cs.bin and requests.jsonl
  maxwell_test_shaders.py <out dir> --service <cmd...>    also runs the translation service (--target spirv) on them,
                                                         validates every .spv with spirv-val (--spirv-val <path>) and
                                                         checks the reflection; exit 0 = all good

Encodings follow vendor/ryujinx Decoders/InstTable.cs + InstDecoders.cs; every instruction carries predicate PT (7).
"""
import json, os, struct, subprocess, sys

RZ = 0xFF
PT = 7 << 16

def ald(rd, attr, size=3):          # ALD rd, a[attr] (size 3 = 128 bits: rd..rd+3)
    return (0b1110111111011 << 51) | PT | rd | (RZ << 8) | (RZ << 39) | (attr << 20) | (size << 47)

def ast(rs, attr, size=3):          # AST a[attr], rs (size 3 = rs..rs+3)
    return (0b1110111111110 << 51) | PT | rs | (RZ << 8) | (RZ << 39) | (attr << 20) | (size << 47)

def ipa_pass(rd, attr):             # IPA.PASS rd, a[attr]
    return (0b11100000 << 56) | PT | rd | (RZ << 8) | (RZ << 20) | (RZ << 39) | (attr << 28)

def texs_2d(rd, rd2, ra, rb, handle, wmask=4):   # TEXS.2D rd, rd2, ra, rb, handle (wmask 4 with rd2 = RGBA)
    return (0b1101100 << 57) | PT | rd | (ra << 8) | (rb << 20) | (rd2 << 28) | (handle << 36) | (1 << 53) | (wmask << 50)

def s2r(rd, sr):                    # S2R rd, SR_<sr> (0x21 = SR_TID.X, 0x25 = SR_CTAID.X)
    return (0b1111000011001 << 51) | PT | rd | (sr << 20)

def mov32i(rd, imm):
    return (0b000000010000 << 52) | PT | rd | ((imm & 0xFFFFFFFF) << 20) | (0xF << 12)

def exit_():
    return (0b111000110000 << 52) | PT | 0xF   # CC.T

def nop():
    return (0b0101000010110 << 51) | PT | (0xF << 8)

SCHED = 0x001F8000FC0007E0   # conservative control word (stall counts); the translator ignores it

def code_words(insts):
    """Interleaves a scheduling word before every group of three instructions."""
    out = []
    insts = list(insts)
    while len(insts) % 3:
        insts.append(nop())
    for i in range(0, len(insts), 3):
        out.append(SCHED); out.extend(insts[i:i + 3])
    out += [SCHED, 0xE2400FFFFF87000F, nop(), nop()]   # trailing BRA-to-self block like real binaries (never reached)
    return b"".join(struct.pack("<Q", w) for w in out)

def sph(stage, imap=None, omap_targets=0):
    """0x50-byte shader program header: type 1 (VTG) / 2 (PS), version 3, stage (1 vertex, 5 fragment)."""
    w = [0] * 20
    sph_type = 2 if stage == 5 else 1
    w[0] = sph_type | (3 << 5) | (stage << 10) | (1 << 14 if stage == 5 else 0)
    if imap:
        for i, b in imap.items():
            w[6 + (i >> 2)] |= b << ((i & 3) * 8)
    w[18] = omap_targets
    return struct.pack("<20I", *w)

def build(out):
    os.makedirs(out, exist_ok=True)
    vs = sph(1) + code_words([ald(0, 0x80), ald(4, 0x90), ast(0, 0x70), ast(4, 0x80), exit_()])
    # fragment: generic input 0 interpolated screen-linear (no 1/w multiply), colour target 0 RGBA from R0..R3
    ps = sph(5, imap={0: 0xFF}, omap_targets=0xF) + code_words([
        ipa_pass(4, 0x80), ipa_pass(5, 0x84), texs_2d(0, 2, 4, 5, handle=8), exit_()])
    cs = code_words([s2r(0, 0x21), s2r(1, 0x25), mov32i(2, 0x3F800000), exit_()])
    for name, data in (("vs.bin", vs), ("ps.bin", ps), ("cs.bin", cs)):
        with open(os.path.join(out, name), "wb") as f:
            f.write(data)
    reqs = [
        {"variant": "test_vs", "stage": "vs", "code": os.path.join(out, "vs.bin"), "c1": "", "attr": {"0": "float", "1": "float"}},
        {"variant": "test_ps", "stage": "ps", "code": os.path.join(out, "ps.bin"), "c1": "", "tex": {"0": "2d"}, "fout": {"0": "float"}},
        {"variant": "test_cs", "stage": "cs", "code": os.path.join(out, "cs.bin"), "c1": "", "local": [64, 1, 1]},
    ]
    with open(os.path.join(out, "requests.jsonl"), "w") as f:
        for r in reqs:
            f.write(json.dumps(r) + "\n")
    return reqs

def main():
    out = sys.argv[1]
    reqs = build(out)
    if "--service" not in sys.argv:
        return 0
    i = sys.argv.index("--service")
    spirv_val = None
    rest = sys.argv[i + 1:]
    if "--spirv-val" in rest:
        j = rest.index("--spirv-val"); spirv_val = rest[j + 1]; rest = rest[:j] + rest[j + 2:]
    cmd = rest + ["--service", "--target", "spirv", "--out", out, "--chain", "test"]
    p = subprocess.run(cmd, input="".join(json.dumps(r) + "\n" for r in reqs), capture_output=True, text=True)
    replies = [json.loads(l) for l in p.stdout.splitlines() if l.strip()]
    bad = 0
    for r, rep in zip(reqs, replies):
        if not rep.get("ok"):
            print(f"FAIL translate {r['variant']}: {rep.get('error')}"); bad += 1; continue
        spv = os.path.join(out, r["variant"] + "_v.spv")
        refl = json.load(open(os.path.join(out, r["variant"] + "_v.json")))
        if spirv_val:
            v = subprocess.run([spirv_val, "--target-env", "vulkan1.3", spv], capture_output=True, text=True)
            if v.returncode:
                print(f"FAIL spirv-val {r['variant']}: {v.stderr.strip()[:400]}"); bad += 1
            else:
                print(f"ok   spirv-val {r['variant']}")
        kinds = sorted((b["kind"], b.get("reg"), b["desc"], b["set"]) for b in refl["bindings"])
        print(f"     {r['variant']}: set {refl['set']} bindings {kinds} vin {refl['vertex_inputs']} fout {refl['frag_outputs']}")
        if r["stage"] == "ps":
            if not any(b["kind"] == "tex" and b.get("reg") == 0 and b["desc"] == "combined" and b.get("samp_kind") == "samp_tex" for b in refl["bindings"]):
                print("FAIL ps: no combined texture slot 0"); bad += 1
            if refl["frag_outputs"] != [0] or refl["set"] != 1:
                print("FAIL ps: outputs / set"); bad += 1
        if r["stage"] == "vs":
            if [v["location"] for v in refl["vertex_inputs"]] != [0, 1] or refl["set"] != 0:
                print("FAIL vs: inputs / set"); bad += 1
    if len(replies) != len(reqs):
        print("FAIL service:", p.stderr[-600:]); bad += 1
    print("PASS" if not bad else f"{bad} failure(s)")
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main())
