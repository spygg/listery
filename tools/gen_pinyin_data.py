"""生成 src/Core/PinyinData.cpp：汉字 -> 主读音 + 异读。

依赖 pypinyin（只在生成数据时需要，构建与运行时都不依赖）：
    python -m pip install pypinyin
    python tools/gen_pinyin_data.py
"""
import pathlib

from pypinyin import Style, pinyin

FIRST = 0x4E00
LAST = 0x9FA5
MAX_READINGS = 3

# 用户实际会用键盘输入的写法：ü 一律写成 v（lv / nve）
def normalize(reading: str) -> str:
    return reading.replace("ü", "v").lower()


rows = []
syllables = set()
for cp in range(FIRST, LAST + 1):
    ch = chr(cp)
    try:
        readings = [normalize(r) for r in pinyin(ch, style=Style.NORMAL, heteronym=True)[0]]
    except Exception:
        readings = []
    seen = []
    for r in readings:
        if r and r not in seen:
            seen.append(r)
    seen = seen[:MAX_READINGS]
    rows.append(seen)
    syllables.update(seen)

ordered = sorted(syllables)
index_of = {s: i + 1 for i, s in enumerate(ordered)}  # 0 表示没有读音
missing = [chr(FIRST + i) for i, r in enumerate(rows) if not r]

primary = [index_of[r[0]] if r else 0 for r in rows]
alternates = []
for offset, r in enumerate(rows):
    for extra in r[1:]:
        alternates.append((offset, index_of[extra]))

out = pathlib.Path(__file__).resolve().parent.parent / "src" / "Core" / "PinyinData.cpp"
lines = []
lines.append("// 由 tools/gen_pinyin_data.py 生成，请勿手改。")
lines.append("// 数据来源：pypinyin（MIT）单字读音表；ü 统一写成 v，方便键盘输入。")
lines.append("#include \"Core/PinyinData.h\"")
lines.append("")
lines.append("#include <algorithm>")
lines.append("")
lines.append("namespace {")
lines.append("const char *const kSyllables[] = {")
for i in range(0, len(ordered), 8):
    chunk = ", ".join(f'"{s}"' for s in ordered[i:i + 8])
    lines.append(f"    {chunk},")
lines.append("};")
lines.append(f"const int kSyllableCount = {len(ordered)};")
lines.append("")
lines.append(f"// U+{FIRST:04X} 起共 {LAST - FIRST + 1} 个汉字，值 = kSyllables 下标 + 1，0 表示无读音")
lines.append(f"const unsigned short kPrimary[{LAST - FIRST + 1}] = {{")
for i in range(0, len(primary), 20):
    chunk = ", ".join(str(v) for v in primary[i:i + 20])
    lines.append(f"    {chunk},")
lines.append("};")
lines.append("")
lines.append("struct AltEntry")
lines.append("{")
lines.append("    unsigned short offset;")
lines.append("    unsigned short syllable;")
lines.append("};")
lines.append("")
lines.append(f"// 异读按 offset 升序，查表时二分定位")
lines.append("const AltEntry kAlternates[] = {")
for i in range(0, len(alternates), 6):
    chunk = ", ".join(f"{{{o}, {s}}}" for o, s in alternates[i:i + 6])
    lines.append(f"    {chunk},")
lines.append("};")
lines.append("") if False else None
lines.append("} // namespace")
lines.append("")
lines.append("namespace PinyinData {")
lines.append("")
lines.append("bool isHan(ushort codePoint)")
lines.append("{")
lines.append(f"    return codePoint >= 0x{FIRST:04X}u && codePoint <= 0x{LAST:04X}u;")
lines.append("}")
lines.append("")
lines.append("QStringList readings(ushort codePoint)")
lines.append("{")
lines.append("    QStringList result;")
lines.append("    if (!isHan(codePoint))")
lines.append("        return result;")
lines.append("    const int offset = codePoint - 0x%04X;" % FIRST)
lines.append("    const unsigned short primary = kPrimary[offset];")
lines.append("    if (primary == 0 || primary > kSyllableCount)")
lines.append("        return result;")
lines.append("    result.append(QString::fromLatin1(kSyllables[primary - 1]));")
lines.append("")
lines.append("    const size_t altCount = sizeof(kAlternates) / sizeof(kAlternates[0]);")
lines.append("    const AltEntry *begin = kAlternates;")
lines.append("    const AltEntry *end = kAlternates + altCount;")
lines.append("    const AltEntry *it = std::lower_bound(")
lines.append("        begin, end, static_cast<unsigned short>(offset),")
lines.append("        [](const AltEntry &entry, unsigned short key) { return entry.offset < key; });")
lines.append("    for (; it != end && it->offset == offset; ++it) {")
lines.append("        if (it->syllable > 0 && it->syllable <= kSyllableCount)")
lines.append("            result.append(QString::fromLatin1(kSyllables[it->syllable - 1]));")
lines.append("    }")
lines.append("    return result;")
lines.append("}")
lines.append("")
lines.append("} // namespace PinyinData")

out.write_text("\n".join(lines) + "\n", encoding="utf-8")
print(f"syllables={len(ordered)} chars={len(rows)} alternates={len(alternates)} missing={len(missing)}")
print("missing sample:", "".join(missing[:20]))
print(f"wrote {out} ({out.stat().st_size} bytes)")
