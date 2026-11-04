#!/usr/bin/env python3
import os
import sys

out_path = sys.argv[1] if len(sys.argv) > 1 else "src/ui/embedded_fonts.hpp"

# If already generated and non-empty, keep it
if os.path.exists(out_path) and os.path.getsize(out_path) > 1000:
    sys.exit(0)

opensans_candidates = [
    os.path.expanduser("~/.local/share/fpshud/fonts/OpenSans-Bold.ttf"),
    "/usr/share/fonts/TTF/OpenSans-Bold.ttf",
    "/usr/share/fonts/truetype/open-sans/OpenSans-Bold.ttf",
    "/usr/share/fonts/open-sans/OpenSans-Bold.ttf",
    "/usr/share/fonts/TTF/DejaVuSansMono-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
]

veramono_candidates = [
    os.path.expanduser("~/.local/share/fpshud/fonts/VeraMoBd.ttf"),
    "/usr/share/fonts/TTF/VeraMoBd.ttf",
    "/usr/share/fonts/truetype/ttf-bitstream-vera/VeraMoBd.ttf",
    "/usr/share/fonts/TTF/DejaVuSansMono-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
]

def read_first_existing(paths):
    for p in paths:
        if os.path.isfile(p):
            with open(p, "rb") as f:
                return f.read()
    return b""

opensans_data = read_first_existing(opensans_candidates)
veramono_data = read_first_existing(veramono_candidates)

def to_c_array(name, data):
    if not data:
        return f"alignas(4) constexpr unsigned char {name}[] = {{ 0x00 }};\nconstexpr bool {name}_valid = false;"
    lines = [f"alignas(4) constexpr unsigned char {name}[] = {{"]
    for i in range(0, len(data), 16):
        chunk = ", ".join(f"0x{b:02x}" for b in data[i:i+16])
        lines.append(f"    {chunk},")
    lines.append("};")
    lines.append(f"constexpr bool {name}_valid = true;")
    return "\n".join(lines)

os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
content = (
    "#pragma once\n\nnamespace fpshud {\n\n"
    + to_c_array("kEmbeddedOpenSansBoldTTF", opensans_data)
    + "\n\n"
    + to_c_array("kEmbeddedVeraMonoBoldTTF", veramono_data)
    + "\n\n} // namespace fpshud\n"
)
with open(out_path, "w") as f:
    f.write(content)
print(f"[*] Generated {out_path} ({len(content)} bytes)")
