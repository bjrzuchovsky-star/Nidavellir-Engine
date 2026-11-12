#!/usr/bin/env python3
"""Cross-compile shaders/rhi at build time.

Authoring language is Vulkan GLSL. glslang writes SPIR-V, SPIRV-Cross writes
HLSL and GLSL 330, and DXC writes DXIL. The generated header is compiled into
the RHI so the runtime does not need the tool chain.
"""

import argparse
import os
import pathlib
import re
import subprocess
import sys


SHADERS = [
    ("mesh.vert", "vert", "mesh.vert", []),
    ("mesh.vert", "vert", "mesh_skin.vert", ["TS_SKIN"]),
    ("mesh.vert", "vert", "foliage.vert", ["TS_FOLIAGE"]),
    ("mesh.frag", "frag", "mesh.frag", []),
    ("mesh.frag", "frag", "foliage.frag", ["TS_FOLIAGE"]),
    ("mesh.frag", "frag", "mesh_rt.frag", ["TS_RAY"]),
    ("depth.vert", "vert", "depth.vert", []),
    ("depth.vert", "vert", "depth_skin.vert", ["TS_SKIN"]),
    ("depth.vert", "vert", "foliage_depth.vert", ["TS_FOLIAGE"]),
    ("depth.frag", "frag", "depth.frag", []),
    ("sky.vert", "vert", "sky.vert", []),
    ("sky.frag", "frag", "sky.frag", []),
    ("line.vert", "vert", "line.vert", []),
    ("line.frag", "frag", "line.frag", []),
    ("bill.vert", "vert", "bill.vert", []),
    ("bill.frag", "frag", "bill.frag", []),
    ("overlay.vert", "vert", "overlay.vert", []),
    ("overlay.frag", "frag", "overlay.frag", []),
    ("fullscreen.vert", "vert", "fullscreen.vert", []),
    ("post.frag", "frag", "post.frag", []),
    ("ssao.frag", "frag", "ssao.frag", []),
    ("bloom.frag", "frag", "bloom.frag", []),
]


def run(cmd, env=None):
    proc = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if proc.returncode != 0:
        sys.stderr.write("$ " + " ".join(cmd) + "\n")
        sys.stderr.write(proc.stdout)
        sys.stderr.write(proc.stderr)
        raise SystemExit(proc.returncode)


def c_ident(name):
    return "k_" + re.sub(r"[^A-Za-z0-9]", "_", name)


def glsl330(text):
    # GL 3.3 has no layout(binding). Block and sampler names stay, and the
    # GL backend binds them by name.
    text = re.sub(r"layout\s*\(\s*binding\s*=\s*\d+\s*,\s*", "layout(", text)
    text = re.sub(r"layout\s*\(\s*binding\s*=\s*\d+\s*\)\s*", "", text)
    return text


def c_string(text):
    out = ['"']
    for ch in text:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append('\\n"\n"')
        else:
            out.append(ch)
    out.append('"')
    return "".join(out)


def emit_u32(f, ident, blob):
    words = len(blob) // 4
    f.write(f"static const unsigned int {ident}[] = {{\n")
    for i in range(words):
        word = int.from_bytes(blob[i * 4 : i * 4 + 4], "little")
        if i % 8 == 0:
            f.write("  ")
        f.write(f"0x{word:08x}u,")
        f.write("\n" if i % 8 == 7 else " ")
    if words % 8 != 0:
        f.write("\n")
    f.write("};\n")
    return words


def emit_u8(f, ident, blob):
    f.write(f"static const unsigned char {ident}[] = {{\n")
    for i, b in enumerate(blob):
        if i % 16 == 0:
            f.write("  ")
        f.write(f"0x{b:02x},")
        f.write("\n" if i % 16 == 15 else " ")
    if len(blob) % 16 != 0:
        f.write("\n")
    f.write("};\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--glslang", required=True)
    ap.add_argument("--spirv-cross", required=True)
    ap.add_argument("--dxc", required=True)
    args = ap.parse_args()
    src = pathlib.Path(args.src)
    out = pathlib.Path(args.out)
    dxc = pathlib.Path(args.dxc)
    dxc_dir = str(dxc.resolve().parent)
    if os.name == "nt":
        missing = [name for name in ("dxcompiler.dll", "dxil.dll") if not (dxc.parent / name).is_file()]
        if missing:
            sys.stderr.write(
                "DXC at %s is missing %s in the same folder.\n"
                "Those DLLs have to match dxc.exe (bin/x64 on AMD64, bin/arm64 on ARM64).\n"
                % (dxc, ", ".join(missing))
            )
            raise SystemExit(1)
    dxc_env = os.environ.copy()
    dxc_env["PATH"] = dxc_dir + os.pathsep + dxc_env.get("PATH", "")
    # Linux packages keep libdxcompiler.so in ../lib (rpath). Prepend both
    # so a copy of dxc without an rpath still loads the matching library.
    if os.name != "nt":
        lib_dir = str(dxc.resolve().parent.parent / "lib")
        dxc_env["LD_LIBRARY_PATH"] = os.pathsep.join(
            [dxc_dir, lib_dir, dxc_env.get("LD_LIBRARY_PATH", "")]
        )
    work = out.parent / "shader_bake"
    work.mkdir(parents=True, exist_ok=True)
    blobs = []
    for source, stage, name, defines in SHADERS:
        spv = work / (name + ".spv")
        hlsl = work / (name + ".hlsl")
        glsl = work / (name + ".glsl")
        dxil = work / (name + ".dxil")
        cmd = [args.glslang, "-V", "--target-env", "vulkan1.2", "-S", stage, "-I" + str(src), str(src / source), "-o", str(spv)]
        for d in defines:
            cmd.append("-D" + d)
        run(cmd)
        run([args.spirv_cross, str(spv), "--hlsl", "--shader-model", "65" if "TS_RAY" in defines else "60", "--output", str(hlsl)])
        if "TS_RAY" in defines:
            # Ray queries are Vulkan / DX12. OpenGL 3.3 has no equivalent stage.
            glsl.write_text("void main(){gl_FragColor=vec4(0.0);}\n", encoding="utf-8")
        else:
            run([args.spirv_cross, str(spv), "--version", "330", "--no-es", "--output", str(glsl)])
        profile = "vs_6_0" if stage == "vert" else ("ps_6_5" if "TS_RAY" in defines else "ps_6_0")
        run([args.dxc, "-T", profile, "-E", "main", "-Fo", str(dxil), str(hlsl)], env=dxc_env)
        blobs.append((name, spv.read_bytes(), glsl330(glsl.read_text(encoding="utf-8")), dxil.read_bytes()))

    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", encoding="utf-8", newline="\n") as f:
        f.write("// Generated by tools/shader_bake/bake.py. Do not edit.\n")
        f.write("#pragma once\n")
        entries = []
        for name, spv, glsl, dxil in blobs:
            ident = c_ident(name)
            words = emit_u32(f, ident + "_spv", spv)
            emit_u8(f, ident + "_dxil", dxil)
            f.write(f"static const char {ident}_glsl[] = {c_string(glsl)};\n")
            entries.append((name, ident, words, len(dxil)))
        f.write("struct TsShaderBlob { const char* name; const char* glsl; const unsigned int* spv; unsigned spv_words; const unsigned char* dxil; unsigned dxil_bytes; };\n")
        f.write("static const TsShaderBlob kTsShaders[] = {\n")
        for name, ident, words, dxil_n in entries:
            f.write(f'  {{"{name}", {ident}_glsl, {ident}_spv, {words}u, {ident}_dxil, {dxil_n}u}},\n')
        f.write("};\n")
        f.write(f"static const unsigned kTsShaderCount = {len(entries)};\n")
    print(f"baked {len(blobs)} shaders -> {out}")


if __name__ == "__main__":
    main()
