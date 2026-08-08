#!/bin/bash
#
# compile_shaders.sh -- Compile Cg shaders to binary .vpo/.fpo files,
# then generate C header with embedded byte arrays.
#
# Two-step process:
#   1. cgc.exe (NVIDIA Cg compiler, 32-bit native) compiles .vcg/.fcg -> assembly
#   2. cgcomp -a (PSL1GHT tool, assembly mode) converts assembly -> .vpo/.fpo
#
# This avoids the 64-bit cgcomp needing a 64-bit cg.dll (which NVIDIA
# no longer distributes).
#
# Requirements:
#   - cgcomp from PSL1GHT (ps3toolchain step 8)
#   - cgc.exe from NVIDIA Cg Toolkit 3.1 (32-bit)
#
# Output: ../ps3gl_shader_data.h (included by ps3gl_shaders.c)

set -e

# ---------- Find cgcomp ----------
CGCOMP="${PS3DEV}/bin/cgcomp"
if [ ! -x "$CGCOMP" ]; then
    CGCOMP="${PS3DEV}/ppu/bin/cgcomp"
fi
if [ ! -x "$CGCOMP" ]; then
    CGCOMP=$(which cgcomp 2>/dev/null || true)
fi

# ---------- Find cgc.exe ----------
# Check common locations for NVIDIA Cg Toolkit
CGC=""
for candidate in \
    "${CGC_PATH:-}" \
    "/c/Program Files (x86)/NVIDIA Corporation/Cg/bin/cgc.exe" \
    "/c/Program Files/NVIDIA Corporation/Cg/bin/cgc.exe" \
    "$(which cgc.exe 2>/dev/null || true)" \
    "$(which cgc 2>/dev/null || true)"; do
    if [ -n "$candidate" ] && [ -x "$candidate" ]; then
        CGC="$candidate"
        break
    fi
done

if [ -z "$CGCOMP" ] || [ ! -x "$CGCOMP" ]; then
    echo "ERROR: cgcomp not found. Build ps3toolchain first."
    echo "Generating stub shader data header..."
    cat > ../ps3gl_shader_data.h <<'STUBEOF'
/*
 * ps3gl_shader_data.h -- STUB: no compiled shaders available.
 * Regenerate with: cd shaders && ./compile_shaders.sh
 */
#ifndef PS3GL_SHADER_DATA_H
#define PS3GL_SHADER_DATA_H

#define PS3GL_SHADERS_AVAILABLE 0

#endif /* PS3GL_SHADER_DATA_H */
STUBEOF
    echo "Wrote stub ps3gl_shader_data.h"
    exit 0
fi

if [ -z "$CGC" ]; then
    echo "ERROR: cgc.exe not found."
    echo "Install NVIDIA Cg Toolkit 3.1 or set CGC= environment variable."
    exit 1
fi

echo "Using cgcomp: $CGCOMP"
echo "Using cgc:    $CGC"

# ---------- Compile shaders: cgc -> assembly -> cgcomp -a -> binary ----------

compile_vp() {
    local src="$1"
    local out="$2"
    local asm="${out}.asm"

    echo "  cgc -profile vp40: $src -> $asm"
    "$CGC" -profile vp40 -o "$asm" "$src"

    echo "  cgcomp -a -v: $asm -> $out"
    "$CGCOMP" -a -v "$asm" "$out"

    rm -f "$asm"
}

compile_fp() {
    local src="$1"
    local out="$2"
    local asm="${out}.asm"

    echo "  cgc -profile fp40: $src -> $asm"
    "$CGC" -profile fp40 -o "$asm" "$src"

    echo "  cgcomp -a -f: $asm -> $out"
    "$CGCOMP" -a -f "$asm" "$out"

    rm -f "$asm"
}

echo ""
echo "Compiling vertex program..."
compile_vp q3_vp.vcg q3_vp.vpo

echo ""
echo "Compiling fragment programs..."
for fp in q3_fp_coloronly q3_fp_modulate q3_fp_replace q3_fp_decal q3_fp_add q3_fp_blend q3_fp_modulate2; do
    compile_fp ${fp}.fcg ${fp}.fpo
done

# ---------- Generate C header with embedded binary data ----------
echo ""
echo "Generating ../ps3gl_shader_data.h..."

python3 - <<'PYEOF'
import os, sys
header_path = "../ps3gl_shader_data.h"
files = [
    ("q3_vp.vpo",           "shader_vp_data"),
    ("q3_fp_coloronly.fpo",  "shader_fp_coloronly_data"),
    ("q3_fp_modulate.fpo",   "shader_fp_modulate_data"),
    ("q3_fp_replace.fpo",    "shader_fp_replace_data"),
    ("q3_fp_decal.fpo",      "shader_fp_decal_data"),
    ("q3_fp_add.fpo",        "shader_fp_add_data"),
    ("q3_fp_blend.fpo",      "shader_fp_blend_data"),
    ("q3_fp_modulate2.fpo",  "shader_fp_modulate2_data"),
]
lines = [
    "/*",
    " * ps3gl_shader_data.h -- Auto-generated embedded shader binaries.",
    " * Do not edit manually. Regenerate with: cd shaders && ./compile_shaders.sh",
    " */",
    "#ifndef PS3GL_SHADER_DATA_H",
    "#define PS3GL_SHADER_DATA_H",
    "",
    "#define PS3GL_SHADERS_AVAILABLE 1",
    "",
]
for fname, varname in files:
    data = open(fname, "rb").read()
    lines.append(f"static const unsigned char {varname}[] __attribute__((aligned(16))) = {{")
    row = []
    for b in data:
        row.append(f"0x{b:02x}")
        if len(row) == 12:
            lines.append("  " + ", ".join(row) + ",")
            row = []
    if row:
        lines.append("  " + ", ".join(row))
    lines.append("};")
    lines.append(f"static const unsigned int {varname}_size = {len(data)};")
    lines.append("")
lines.append("#endif /* PS3GL_SHADER_DATA_H */")
with open(header_path, "w") as f:
    f.write("\n".join(lines) + "\n")
print(f"Done. {len(lines)} lines written to {header_path}")
PYEOF

echo ""
echo "Shader binaries:"
ls -la *.vpo *.fpo 2>/dev/null
