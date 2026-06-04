#include "softdips.h"
#include <fstream>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <map>
#include <sstream>
#include <cstdio>
#include <regex>

namespace softdips {

static std::string trim(const std::string& str) {
    size_t start = str.find_first_not_of(' ');
    if (start == std::string::npos) return "";
    size_t end = str.find_last_not_of(' ');
    return str.substr(start, end - start + 1);
}

uint16_t SoftDipsParser::swap16(uint16_t value) {
    return ((value & 0xFF) << 8) | ((value >> 8) & 0xFF);
}

void SoftDipsParser::byteSwap16(uint8_t* data, size_t length) {
    for (size_t i = 0; i + 1 < length; i += 2) {
        std::swap(data[i], data[i + 1]);
    }
}

static constexpr size_t COLUMN_WIDTH = 12;

// Marks the start of a "NEXT PAGE" page-break entry in the text region.
static constexpr uint8_t PAGE_BREAK_MARKER = 0x13;

// A NeoGeo soft DIP table describes its switches in a fixed 16-byte metadata
// block that follows the 16-byte game title (see neogeodev wiki "Software dip
// switches"):
//
//   bytes 0-5   "special list" - 4 typed entries, each with a name string but
//                 no stored option strings (the BIOS renders the value):
//                   0-1  time setting 1   (BCD MM:SS word; FFFF = unused)
//                   2-3  time setting 2   (BCD MM:SS word; FFFF = unused)
//                   4    count setting 1  (byte 1-99;        FF   = unused)
//                   5    count setting 2  (byte 0-100: 0=WITHOUT,
//                        1-99, 100=INFINITE;            FF   = unused)
//   bytes 6-15  "option header" - up to 10 simple settings whose option strings
//                 are stored in the text. Each byte: low nibble = option count,
//                 high nibble = current option index; 0 count = empty / page
//                 break. These are "List" settings.
//
// In the text, used special settings come first (in order), each a single name
// field, then the simple settings (name field + option fields).

static int bcdToDec(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }
static uint8_t decToBcd(int n)  { return uint8_t(((n / 10) << 4) | (n % 10)); }

// Constants for the two count settings' value ranges.
static constexpr int kCount2Infinite = 100;  // displayed as "INFINITE"

// Fill a count switch's options and current index from its value.
static void buildCountOptions(DipSwitch& sw, int value) {
    if (sw.kind == DipSwitch::Kind::Count1) {
        for (int i = 1; i <= 99; i++) sw.options.push_back({std::to_string(i)});
        sw.currentIndex = std::clamp(value - 1, 0, 98);
    } else {  // Count2
        sw.options.push_back({"WITHOUT"});
        for (int i = 1; i <= 99; i++) sw.options.push_back({std::to_string(i)});
        sw.options.push_back({"INFINITE"});
        sw.currentIndex = std::clamp(value, 0, kCount2Infinite);
    }
    sw.defaultIndex = sw.currentIndex;
}

// Read the 12-byte field at `fieldIndex` from the raw (big-endian, 0x13 intact)
// text region. Returns false if the field runs past the end of the data.
static bool readField(const std::string& text, size_t fieldIndex,
                      std::string& trimmed, bool& isPageBreak) {
    size_t off = fieldIndex * COLUMN_WIDTH;
    if (off >= text.size()) return false;
    std::string field = text.substr(off, COLUMN_WIDTH);
    field.resize(COLUMN_WIDTH, ' ');
    // The 0x13 page-break marker may sit anywhere in the field. Some games put
    // it on the divider's name field (e.g. "\x13 NEXT PAGE"), others on the
    // divider's option field (the name then reads plainly as "NEXT").
    isPageBreak = field.find(static_cast<char>(PAGE_BREAK_MARKER)) != std::string::npos;
    // Drop control bytes (the 0x13 marker) before trimming whitespace.
    field.erase(std::remove_if(field.begin(), field.end(),
                               [](char c) { return static_cast<uint8_t>(c) < 0x20; }),
                field.end());
    trimmed = trim(field);
    return true;
}

// Build the switch sections directly from the metadata block. This is fully
// deterministic: the metadata tells us exactly how many switches there are and
// how many options each one has, so there is no guessing which 12-byte field
// is a name versus an option.
static std::vector<DipSwitchSection> parseTable(const std::string& text,
                                                const uint8_t special[6],
                                                const uint8_t header[10]) {
    std::vector<DipSwitchSection> sections;
    sections.emplace_back();
    size_t field = 0;

    auto readName = [&](std::string& name) {
        bool pb = false;
        if (!readField(text, field, name, pb)) return false;
        field++;
        return true;
    };

    // ── Special settings (time1, time2, count1, count2), used ones in order ──
    struct Sp { DipSwitch::Kind kind; int off; int value; };
    std::vector<Sp> specials;
    uint16_t t1 = (uint16_t(special[0]) << 8) | special[1];
    uint16_t t2 = (uint16_t(special[2]) << 8) | special[3];
    if (t1 != 0xFFFF) specials.push_back({DipSwitch::Kind::Time, 0, t1});
    if (t2 != 0xFFFF) specials.push_back({DipSwitch::Kind::Time, 2, t2});
    if (special[4] != 0xFF) specials.push_back({DipSwitch::Kind::Count1, 4, special[4]});
    if (special[5] != 0xFF) specials.push_back({DipSwitch::Kind::Count2, 5, special[5]});

    for (const auto& s : specials) {
        std::string name;
        if (!readName(name)) break;
        if (name.empty()) continue;

        if (s.kind == DipSwitch::Kind::Time) {
            // Split a BCD MM:SS word into separate minutes and seconds dropdowns
            // (matching the BackBit / Unibios editors).
            int mins = bcdToDec(uint8_t(s.value >> 8));
            int secs = bcdToDec(uint8_t(s.value & 0xFF));
            DipSwitch mm;
            mm.name = name + " (MIN)";
            mm.kind = DipSwitch::Kind::Time;
            mm.metaByteIndex = s.off;
            mm.timeField = 0;
            for (int i = 0; i <= 29; i++) mm.options.push_back({std::to_string(i)});
            mm.currentIndex = mm.defaultIndex = std::clamp(mins, 0, 29);
            sections.back().switches.push_back(std::move(mm));

            DipSwitch ss;
            ss.name = name + " (SEC)";
            ss.kind = DipSwitch::Kind::Time;
            ss.metaByteIndex = s.off;
            ss.timeField = 1;
            for (int i = 0; i < 60; i++) ss.options.push_back({std::to_string(i)});
            ss.currentIndex = ss.defaultIndex = std::clamp(secs, 0, 59);
            sections.back().switches.push_back(std::move(ss));
        } else {
            DipSwitch sw;
            sw.name = name;
            sw.kind = s.kind;
            sw.metaByteIndex = s.off;
            buildCountOptions(sw, s.value);
            sections.back().switches.push_back(std::move(sw));
        }
    }

    // ── Simple (List) settings from the option header ──
    for (int i = 0; i < 10; i++) {
        int count = header[i] & 0x0F;
        int sel   = (header[i] >> 4) & 0x0F;
        if (count == 0) continue;  // empty slot / padding

        std::string name;
        bool nameHasMarker = false;
        if (!readField(text, field, name, nameHasMarker)) break;
        field++;

        // A page break is a count-1 pseudo-setting labelled "NEXT"/"NEXT PAGE"
        // and/or carrying the 0x13 marker (on its name or its option field).
        if (nameHasMarker || name == "NEXT" || name.rfind("NEXT ", 0) == 0) {
            field += count;  // consume its (blank) option field(s)
            if (!sections.back().switches.empty()) sections.emplace_back();
            continue;
        }

        DipSwitch sw;
        sw.name = name;
        sw.kind = DipSwitch::Kind::List;
        sw.metaByteIndex = 6 + i;
        for (int k = 0; k < count; k++) {
            std::string opt;
            bool pb = false;
            if (!readField(text, field, opt, pb)) break;
            field++;
            sw.options.push_back({opt});
        }
        int n = (int)sw.options.size();
        sw.currentIndex = (n > 0) ? std::min(sel, n - 1) : 0;
        sw.defaultIndex = sw.currentIndex;

        if (!sw.name.empty() && !sw.options.empty())
            sections.back().switches.push_back(std::move(sw));
    }

    // Drop a trailing empty section produced by a final page break.
    while (!sections.empty() && sections.back().switches.empty())
        sections.pop_back();
    return sections;
}

// Split a string on '/' into trimmed fields.
static std::vector<std::string> splitSlash(const std::string& s) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        size_t pos = s.find('/', start);
        if (pos == std::string::npos) { parts.push_back(trim(s.substr(start))); break; }
        parts.push_back(trim(s.substr(start, pos - start)));
        start = pos + 1;
    }
    return parts;
}

// If `sw` is a packed switch ("CREDIT/LEVEL" with options like "ON/OFF" that
// form a complete cross-product of per-field values), produce its sub-switches
// and the PackGroup needed to re-combine them on write. Returns false if `sw`
// is not safely decomposable (then it is left as a single switch).
static bool tryDecompose(const DipSwitch& sw, int groupId,
                         std::vector<DipSwitch>& outSubs, PackGroup& outPg) {
    if (sw.kind != DipSwitch::Kind::List) return false;
    auto nameParts = splitSlash(sw.name);
    const size_t N = nameParts.size();
    if (N < 2) return false;
    for (const auto& np : nameParts) if (np.empty()) return false;

    // Split every option into exactly N fields; collect the tuples.
    std::vector<std::vector<std::string>> tuples;
    for (const auto& opt : sw.options) {
        auto fields = splitSlash(opt.name);
        if (fields.size() != N) return false;
        tuples.push_back(std::move(fields));
    }

    // Per-field distinct values in first-seen order.
    std::vector<std::vector<std::string>> fieldValues(N);
    auto indexOf = [](const std::vector<std::string>& v, const std::string& s) {
        for (size_t i = 0; i < v.size(); i++) if (v[i] == s) return (int)i;
        return -1;
    };
    for (const auto& t : tuples)
        for (size_t f = 0; f < N; f++)
            if (indexOf(fieldValues[f], t[f]) < 0) fieldValues[f].push_back(t[f]);

    // Require a complete cross-product (so re-combining is unambiguous) and at
    // least two choices per field (otherwise there is nothing to toggle).
    size_t product = 1;
    for (size_t f = 0; f < N; f++) {
        if (fieldValues[f].size() < 2) return false;
        product *= fieldValues[f].size();
    }
    if (product != tuples.size()) return false;
    // Distinct tuples + count == product ⇒ it is the full cross-product.
    for (size_t i = 0; i < tuples.size(); i++)
        for (size_t j = i + 1; j < tuples.size(); j++)
            if (tuples[i] == tuples[j]) return false;

    const auto& curTuple = tuples[std::min<size_t>(sw.currentIndex, tuples.size() - 1)];
    const auto& defTuple = tuples[std::min<size_t>(sw.defaultIndex, tuples.size() - 1)];

    for (size_t f = 0; f < N; f++) {
        DipSwitch sub;
        sub.name = nameParts[f];
        sub.metaByteIndex = sw.metaByteIndex;
        sub.packGroup = groupId;
        sub.packField = (int)f;
        for (const auto& v : fieldValues[f]) sub.options.push_back({v});
        sub.currentIndex = std::max(0, indexOf(fieldValues[f], curTuple[f]));
        sub.defaultIndex = std::max(0, indexOf(fieldValues[f], defTuple[f]));
        outSubs.push_back(std::move(sub));
    }
    outPg.metaByteIndex = sw.metaByteIndex;
    outPg.combos = std::move(tuples);
    return true;
}

// Replace packed switches in every section with their sub-switches, recording
// the PackGroups on the file so writes can re-combine them.
static void decomposeSections(SoftDipsFile& f) {
    for (auto& section : f.sections) {
        std::vector<DipSwitch> rebuilt;
        for (auto& sw : section.switches) {
            std::vector<DipSwitch> subs;
            PackGroup pg;
            if (tryDecompose(sw, (int)f.packGroups.size(), subs, pg)) {
                f.packGroups.push_back(std::move(pg));
                for (auto& s : subs) rebuilt.push_back(std::move(s));
            } else {
                rebuilt.push_back(std::move(sw));
            }
        }
        section.switches = std::move(rebuilt);
    }
}

std::optional<SoftDipsFile> SoftDipsParser::parse(const std::filesystem::path& filePath) {
    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return std::nullopt;

    size_t fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> data(fileSize);
    if (!file.read(reinterpret_cast<char*>(data.data()), fileSize)) return std::nullopt;
    return parse(data);
}

std::optional<SoftDipsFile> SoftDipsParser::parse(const std::vector<uint8_t>& data) {
    if (data.size() < 32) return std::nullopt;

    SoftDipsFile result;
    std::vector<uint8_t> swapped = data;
    byteSwap16(swapped.data(), swapped.size());

    result.gameName = trim(std::string(swapped.begin(), swapped.begin() + 16));
    std::memcpy(result.headerMarker, swapped.data() + 16, 6);
    std::memcpy(result.settingsData, swapped.data() + 22, 10);

    size_t textStart = 32;
    if (swapped.size() > textStart) {
        result.rawText.assign(swapped.begin() + textStart, swapped.end());
        std::string text(swapped.begin() + textStart, swapped.end());
        result.sections = parseTable(text, result.headerMarker, result.settingsData);
        decomposeSections(result);
    }
    return result;
}

std::vector<uint8_t> SoftDipsParser::toBytes(const SoftDipsFile& file) {
    // Start from the canonical metadata and write each switch's current value
    // back into its controlling byte(s). List settings keep their low nibble
    // (option count) and patch the high nibble; special settings write the full
    // count byte / BCD time word. The text region is emitted verbatim.
    uint8_t special[6];
    uint8_t header[10];
    std::memcpy(special, file.headerMarker, 6);
    std::memcpy(header, file.settingsData, 10);

    auto patchNibble = [&](int idx, int sel) {
        sel &= 0x0F;
        if (idx >= 6 && idx < 16) {
            uint8_t& b = header[idx - 6];
            b = static_cast<uint8_t>((sel << 4) | (b & 0x0F));
        }
    };

    // Collect the chosen value of each packed sub-switch, keyed by pack group.
    std::map<int, std::map<int, std::string>> packSel;  // group -> field -> value
    // Time settings are written as a BCD word; gather both halves (min/sec) by
    // their shared byte offset before encoding.
    std::map<int, std::pair<int, int>> timeVals;  // off -> (minutes, seconds)

    for (const auto& section : file.sections) {
        for (const auto& sw : section.switches) {
            if (sw.packGroup >= 0) {
                if (sw.currentIndex >= 0 && sw.currentIndex < (int)sw.options.size())
                    packSel[sw.packGroup][sw.packField] = sw.options[sw.currentIndex].name;
                continue;  // re-combined below
            }
            int idx = sw.metaByteIndex;
            switch (sw.kind) {
                case DipSwitch::Kind::List:
                    patchNibble(idx, sw.currentIndex);
                    break;
                case DipSwitch::Kind::Count1:  // value 1-99
                    special[4] = uint8_t(std::clamp(sw.currentIndex + 1, 1, 99));
                    break;
                case DipSwitch::Kind::Count2:  // 0=WITHOUT, 1-99, 100=INFINITE
                    special[5] = uint8_t(std::clamp(sw.currentIndex, 0, kCount2Infinite));
                    break;
                case DipSwitch::Kind::Time:
                    if (idx == 0 || idx == 2) {
                        if (sw.timeField == 0) timeVals[idx].first  = sw.currentIndex;
                        else                   timeVals[idx].second = sw.currentIndex;
                    }
                    break;
            }
        }
    }

    for (const auto& [off, ms] : timeVals) {
        special[off]     = decToBcd(std::clamp(ms.first, 0, 29));
        special[off + 1] = decToBcd(std::clamp(ms.second, 0, 59));
    }

    // Re-combine each packed group: build the chosen field tuple and find the
    // matching combined option index, which becomes the byte's high nibble.
    for (const auto& [g, fields] : packSel) {
        if (g >= (int)file.packGroups.size()) continue;
        const PackGroup& pg = file.packGroups[g];
        for (size_t k = 0; k < pg.combos.size(); k++) {
            const auto& combo = pg.combos[k];
            bool match = combo.size() == fields.size();
            for (const auto& [f, val] : fields) {
                if (f < 0 || f >= (int)combo.size() || combo[f] != val) { match = false; break; }
            }
            if (match) { patchNibble(pg.metaByteIndex, (int)k); break; }
        }
    }

    std::vector<uint8_t> result(32, ' ');
    std::string namePadded = file.gameName;
    namePadded.resize(16, ' ');
    std::memcpy(result.data(), namePadded.c_str(), 16);
    std::memcpy(result.data() + 16, special, 6);
    std::memcpy(result.data() + 22, header, 10);

    result.insert(result.end(), file.rawText.begin(), file.rawText.end());

    byteSwap16(result.data(), result.size());
    return result;
}

bool SoftDipsParser::write(const std::filesystem::path& filePath, const SoftDipsFile& file) {
    auto data = toBytes(file);
    std::ofstream out(filePath, std::ios::binary);
    if (!out.is_open()) return false;
    return out.write(reinterpret_cast<const char*>(data.data()), data.size()).good();
}

static std::string toLower(std::string s) {
    for (char& c : s) c = (c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c;
    return s;
}

// Does `stem` (filename without its final extension) end with a program-ROM
// token? MAME program ROMs name the maincpu chip many ways — p1, p1a, p1sp,
// p1pa, p1pl, p1up, pg1, ph1, pn1, pk1, hp1, ep1, and the rare p2/p2a (a few
// sets load p2 first). The token must start the stem or follow a separator so
// we don't match unrelated names that merely end in "...p1".
static bool endsWithProgramToken(const std::string& stem) {
    // (sep|start)(letter group)(bank digit)(optional revision/region suffix)
    static const std::regex re(
        R"((^|[-_. ])(ep|hp|ph|pg|pn|pk|sp|p)[0-9][0-9a-z]*$)",
        std::regex::icase);
    return std::regex_search(stem, re);
}

// Of the program-ROM stems above, is this the *first* program ROM (bank 1, which
// holds the softdips header) rather than a later bank (p2, p3, ...)? Used only to
// try the first PROM before falling back to later ones.
static bool isFirstBankToken(const std::string& stem) {
    // p1-class: the bank digit is 1 (p1, pg1, ph1, hp1, ep1, p1sp, ...).
    static const std::regex bank1(
        R"((^|[-_. ])(ep|hp|ph|pg|pn|pk|p)1[0-9a-z]*$)", std::regex::icase);
    // BackBit/chip names without a bank digit are first by convention (pg, prom).
    static const std::regex noDigit(
        R"((^|[-_. ])(pg|ph|pn|pk))", std::regex::icase);
    return std::regex_search(stem, bank1) || std::regex_search(stem, noDigit);
}

std::vector<std::string> SoftDipsParser::rankProgramRoms(
    const std::vector<std::string>& filenames) {
    namespace fs = std::filesystem;
    // Each candidate gets a sort key {bank, naming, variant, name}, lower tried
    // first. The softdips header lives in the *first* program ROM (the one that
    // loads at offset 0 in MAME's cslot1:maincpu), so bank is the primary key:
    //   bank   : 0 = first program ROM (p1-class), 1 = a later bank (p2-class).
    //   naming : 0 = BackBit/chip extension (.p1/.pd/.ep1/.p2/.sp2), 1 = MAME .bin/.rom.
    //   variant: 0 = canonical ("prom", "<id>-p1"), 1 = revision/hack form.
    struct Cand { int bank; int naming; int variant; std::string name; std::string file; };
    std::vector<Cand> cands;

    for (const auto& fn : filenames) {
        // path is used only to split name/extension here — no filesystem access.
        fs::path p(fn);
        std::string ext = toLower(p.extension().string());
        std::string stem = toLower(p.stem().string());

        int naming, bank;
        if (ext == ".p1" || ext == ".pd" || ext == ".ep1") {
            naming = 0; bank = 0;                 // first PROM by chip extension
        } else if (ext == ".p2" || ext == ".sp2" || ext == ".ep2") {
            naming = 0; bank = 1;                 // later PROM (a few sets load it first)
        } else if ((ext == ".bin" || ext == ".rom") && endsWithProgramToken(stem)) {
            naming = 1; bank = isFirstBankToken(stem) ? 0 : 1;   // MAME naming
        } else {
            continue;                             // not a program ROM
        }
        int variant = (stem == "prom" || isFirstBankToken(stem)) ? 0 : 1;
        cands.push_back({bank, naming, variant, stem, fn});
    }

    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        if (a.bank    != b.bank)    return a.bank    < b.bank;
        if (a.naming  != b.naming)  return a.naming  < b.naming;
        if (a.variant != b.variant) return a.variant < b.variant;
        return a.name < b.name;
    });

    std::vector<std::string> result;
    for (const auto& c : cands) result.push_back(c.file);
    return result;
}

std::vector<std::filesystem::path> SoftDipsParser::findProgramRoms(
    const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return {};

    std::vector<std::string> names;
    std::map<std::string, fs::path> byName;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        std::string fn = entry.path().filename().string();
        names.push_back(fn);
        byName[fn] = entry.path();
    }

    std::vector<fs::path> result;
    for (const auto& n : rankProgramRoms(names)) result.push_back(byName[n]);
    return result;
}

// A genuine softdips table is pure printable ASCII; reject any switch whose
// name or option labels contain non-text bytes (a sign of a false-positive
// match inside code/graphics data).
static bool looksLikeTextTable(const SoftDipsFile& f) {
    auto printable = [](const std::string& s) {
        if (s.empty()) return false;
        for (unsigned char c : s) if (c < 0x20 || c > 0x7E) return false;
        return true;
    };
    int count = 0;
    for (const auto* sw : f.getAllSwitches()) {
        if (!printable(sw->name)) return false;
        for (const auto& opt : sw->options)
            if (!printable(opt.name)) return false;
        count++;
    }
    return count > 0;
}

// Try to read a softdips table whose 16-byte title begins at `start` in the
// byte-swapped (big-endian) ROM image. Returns a validated table or nullopt.
static std::optional<SoftDipsFile> tryExtractTableAt(const std::vector<uint8_t>& rom,
                                                     size_t start) {
    if (start < 16 || start + 48 > rom.size()) return std::nullopt;

    // Find the end of the text section (a control byte terminates it).
    size_t end = start + 32;
    while (end < rom.size()) {
        uint8_t c = rom[end];
        if (c <= 7) break;  // 0x00-0x07 control bytes terminate the text section
        if (c >= 32 && c < 127) { end++; continue; }
        if (c == 0x13) { end++; continue; }
        if (c == 0xFF && (end - start) < 100) { end++; continue; }
        break;
    }
    if (end - start < 48) return std::nullopt;

    std::vector<uint8_t> extracted(rom.begin() + start, rom.begin() + end);
    SoftDipsParser::byteSwap16(extracted.data(), extracted.size());
    auto result = SoftDipsParser::parse(extracted);
    if (!result || result->sections.empty()) return std::nullopt;

    std::string gn = result->gameName;
    if (gn.size() < 3) return std::nullopt;
    bool allSame = true;
    for (size_t k = 1; k < gn.size(); k++) if (gn[k] != gn[0]) { allSame = false; break; }
    if (allSame) return std::nullopt;
    if (!looksLikeTextTable(*result)) return std::nullopt;
    return result;
}

std::optional<SoftDipsFile> SoftDipsParser::extractFromRom(const std::filesystem::path& romPath) {
    return extractFromRom(romPath, nullptr);
}

std::optional<SoftDipsFile> SoftDipsParser::extractFromRom(
    const std::filesystem::path& romPath, std::string* diagnostics) {
    std::ifstream file(romPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        if (diagnostics) *diagnostics = "Could not open ROM file";
        return std::nullopt;
    }

    size_t fileSize = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> romData(fileSize);
    if (!file.read(reinterpret_cast<char*>(romData.data()), fileSize)) {
        if (diagnostics) *diagnostics = "Could not read ROM file";
        return std::nullopt;
    }
    return extractFromRom(romData, diagnostics);
}

std::optional<SoftDipsFile> SoftDipsParser::extractFromRom(
    const std::vector<uint8_t>& romBytes, std::string* diagnostics) {
    // Byte-swap a working copy of the ROM to get big-endian data.
    std::vector<uint8_t> romData = romBytes;
    byteSwap16(romData.data(), romData.size());

    // Search for softdips: header marker is 6 bytes, first 4 must be 0xFF
    for (size_t i = 22; i + 64 < romData.size(); i++) {
        // Require first 4 bytes to be 0xFF; bytes 4-5 may vary (some games have 03 FF)
        if (romData[i] != 0xFF || romData[i+1] != 0xFF ||
            romData[i+2] != 0xFF || romData[i+3] != 0xFF) {
            continue;
        }

        if (i < 16) continue;

        // Check 16 bytes before for a valid game name.
        std::string name;
        int alphaCount = 0;
        int uniqueChars = 0;
        bool validName = true;
        for (int j = 0; j < 16; j++) {
            uint8_t c = romData[i - 16 + j];
            if (c >= 'A' && c <= 'Z') {
                alphaCount++;
                name += (char)c;
            } else if (c >= '0' && c <= '9') {
                name += (char)c;
            } else if (c == ' ') {
                name += ' ';
            } else if (c == '\'' || c == '.' || c == '-' || c == '&' ||
                       c == '!' || c == ':' || c == '/') {
                // Punctuation that legitimately appears in titles, e.g.
                // "NINJA MASTER'S", "P.O.W.", "SUPER SIDEKICKS".
                name += (char)c;
            } else {
                validName = false;
                break;
            }
        }

        // Count unique uppercase letters in the name
        {
            std::string seen;
            for (char c : name) {
                if (c >= 'A' && c <= 'Z' && seen.find(c) == std::string::npos) {
                    seen += c;
                }
            }
            uniqueChars = (int)seen.size();
        }

        // Reject if too few letters, or all same char (like "QQQQQQQQQQQQ"),
        // or if name is too short after trimming (like "NO")
        std::string trimmedName;
        {
            size_t a = name.find_first_not_of(' ');
            size_t b = name.find_last_not_of(' ');
            if (a != std::string::npos) {
                trimmedName = name.substr(a, b - a + 1);
            }
        }

        if (!validName || alphaCount < 3 || uniqueChars < 3 || trimmedName.size() < 3) {
            continue;
        }

        // Now find the end of the data.
        size_t start = i - 16;
        size_t end = start + 32;

        while (end < romData.size()) {
            uint8_t c = romData[end];

            // Control characters (0x00-0x07) = end of text section
            if (c <= 7) break;

            // Valid text characters
            if (c >= 32 && c < 127) { end++; continue; }

            // Allow 0x13 marker
            if (c == 0x13) { end++; continue; }

            // Allow 0xFF only within the first 100 bytes
            if (c == 0xFF && (end - start) < 100) { end++; continue; }

            // Anything else = end of text section
            break;
        }

        // Require minimum size to be a valid softdips block
        if (end - start < 48) continue;

        // Extract the data
        std::vector<uint8_t> extracted(romData.begin() + start, romData.begin() + end);

        // For the .softdips format, we need to byte-swap back
        byteSwap16(extracted.data(), extracted.size());

        auto result = parse(extracted);

        // Reject if parse failed, or if the result has no dip switches
        if (!result || result->sections.empty()) {
            continue;
        }

        // Verify the game name looks legitimate (not all same char like "QQQQQQQQQQQQ")
        {
            std::string gn = result->gameName;
            if (gn.size() < 3) continue;
            bool allSame = true;
            char firstChar = gn[0];
            for (size_t k = 1; k < gn.size(); k++) {
                if (gn[k] != firstChar) { allSame = false; break; }
            }
            if (allSame) continue;
        }

        // Reject false positives: a real table's switch names and option labels
        // are printable ASCII. Binary garbage (e.g. a stray FF run in code) often
        // parses into a plausible-looking title but non-text option fields.
        if (!looksLikeTextTable(*result)) continue;

        return result;
    }

    // Fallback: locate the table through the 68k program header's region
    // pointers (Japan/US/Europe). This finds tables the FF-marker scan misses —
    // e.g. kotm, whose metadata block doesn't start with 0xFF bytes.
    if (romData.size() > 0x122) {
        auto be32 = [&](size_t o) {
            return (uint32_t(romData[o]) << 24) | (uint32_t(romData[o + 1]) << 16) |
                   (uint32_t(romData[o + 2]) << 8) | uint32_t(romData[o + 3]);
        };
        for (size_t ptrOff : {size_t(0x11a), size_t(0x116), size_t(0x11e)}) {
            uint32_t p = be32(ptrOff);
            auto result = tryExtractTableAt(romData, p);
            if (result) return result;
        }
    }

    if (diagnostics) {
        *diagnostics = "No soft DIP settings found in this program ROM "
                        "(the game may have none).";
    }
    return std::nullopt;
}

std::optional<SoftDipsFile> SoftDipsParser::extractFromDir(
    const std::filesystem::path& dir, std::string* diagnostics) {
    auto roms = findProgramRoms(dir);
    if (roms.empty()) {
        if (diagnostics) *diagnostics = "No program ROM found";
        return std::nullopt;
    }
    std::string lastDiag;
    for (const auto& rom : roms) {
        auto extracted = extractFromRom(rom, &lastDiag);
        if (extracted && !extracted->sections.empty()) return extracted;
    }
    if (diagnostics) {
        *diagnostics = lastDiag.empty() ? "No softdips table in program ROM" : lastDiag;
    }
    return std::nullopt;
}

std::vector<std::string> SoftDipsParser::compareStructure(
    const SoftDipsFile& softdips, const SoftDipsFile& fromRom) {
    std::vector<std::string> diffs;
    constexpr size_t kMaxDiffs = 16;
    auto note = [&](const std::string& msg) {
        if (diffs.size() < kMaxDiffs) diffs.push_back(msg);
    };

    if (trim(softdips.gameName) != trim(fromRom.gameName)) {
        note("game name: \"" + softdips.gameName + "\" vs ROM \"" + fromRom.gameName + "\"");
    }

    auto a = softdips.getAllSwitches();
    auto b = fromRom.getAllSwitches();
    if (a.size() != b.size()) {
        note("switch count: " + std::to_string(a.size()) +
             " vs ROM " + std::to_string(b.size()));
    }

    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; i++) {
        const DipSwitch* sa = a[i];
        const DipSwitch* sb = b[i];
        if (sa->name != sb->name) {
            note("#" + std::to_string(i) + " name: \"" + sa->name +
                 "\" vs ROM \"" + sb->name + "\"");
            continue;  // options below would just be noise once names diverge
        }
        if (sa->options.size() != sb->options.size()) {
            note("\"" + sa->name + "\" option count: " +
                 std::to_string(sa->options.size()) + " vs ROM " +
                 std::to_string(sb->options.size()));
            continue;
        }
        for (size_t k = 0; k < sa->options.size(); k++) {
            if (sa->options[k].name != sb->options[k].name) {
                note("\"" + sa->name + "\" option " + std::to_string(k) + ": \"" +
                     sa->options[k].name + "\" vs ROM \"" + sb->options[k].name + "\"");
                break;
            }
        }
    }
    return diffs;
}

AuditResult SoftDipsParser::auditGameDir(const std::filesystem::path& gameDir) {
    AuditResult result;
    std::filesystem::path softPath = gameDir / ".softdips";

    std::error_code ec;
    bool hasSoft = std::filesystem::exists(softPath, ec);

    std::string romDiag;
    auto fromRom = extractFromDir(gameDir, &romDiag);

    if (!hasSoft) {
        result.status = AuditResult::Status::NoSoftDips;
        if (fromRom) result.gameName = fromRom->gameName;
        return result;
    }

    auto soft = parse(softPath);
    if (!soft) {
        result.status = AuditResult::Status::SoftDipsParseFailed;
        return result;
    }
    result.gameName = soft->gameName;

    if (!fromRom) {
        result.status = AuditResult::Status::NoProgramRom;
        // Distinguish "no rom at all" from "rom present but unreadable table".
        if (!findProgramRoms(gameDir).empty())
            result.status = AuditResult::Status::RomExtractFailed;
        return result;
    }

    result.differences = compareStructure(*soft, *fromRom);
    result.status = result.differences.empty()
        ? AuditResult::Status::Ok
        : AuditResult::Status::StructureMismatch;
    return result;
}

std::string SoftDipsParser::normalizeName(const std::string& raw) {
    std::string up;
    for (char c : raw) {
        if (c == '.' || c == ':') continue;
        if (c == '/') { up += ' '; continue; }  // "DSP/CREDIT" -> "DSP CREDIT"
        up += (c >= 'a' && c <= 'z') ? char(c - 'a' + 'A') : c;
    }
    // Tokenize on whitespace, drop on-screen-display indicator words, LVL->LEVEL.
    std::istringstream iss(up);
    std::string tok, out;
    while (iss >> tok) {
        if (tok == "DISP" || tok == "DSP" || tok == "DISPLAY" || tok == "INDI" ||
            tok == "INDIC" || tok == "INDICATOR")
            continue;
        if (tok == "LVL") tok = "LEVEL";
        if (!out.empty()) out += " ";
        out += tok;
    }

    // Canonicalize known synonyms so equivalent settings match across games.
    static const std::vector<std::pair<std::string, std::string>> kSynonyms = {
        {"INSTRUCTION",  "HOW TO PLAY"},
        {"INSTRUCTIONS", "HOW TO PLAY"},
        {"HOW TO",       "HOW TO PLAY"},  // samsho's packed "HOW TO/DEMO" half
        {"PLAY MANUAL",  "HOW TO PLAY"},
    };
    for (const auto& [from, to] : kSynonyms)
        if (out == from) { out = to; break; }
    return out;
}

std::string SoftDipsParser::normalizeValue(const std::string& raw) {
    std::string s;
    for (char c : raw) {
        if (c == ' ' || c == '.' || c == ':') continue;
        s += (c >= 'a' && c <= 'z') ? char(c - 'a' + 'A') : c;
    }
    if (s.rfind("DISP", 0) == 0) s = s.substr(4);  // "DISP.ON" -> "ON"
    if (s == "OFF" || s == "WITHOUT" || s == "NO") return "DISABLE";
    if (s == "ON" || s == "WITH" || s == "YES") return "ENABLE";
    return s;  // some other literal value (e.g. a difficulty level)
}

CloneMatch SoftDipsParser::matchSetting(const SoftDipsFile& game,
                                          const std::string& conceptName,
                                          const std::string& desiredValue) {
    CloneMatch result;
    const std::string wantName = normalizeName(conceptName);
    const std::string wantVal  = normalizeValue(desiredValue);

    std::vector<CloneMatch::Candidate> valueMatches;   // switch+option that can take the value
    std::vector<CloneMatch::Candidate> nameOnly;       // name matched, no compatible value
    bool internallyAmbiguous = false;                    // a switch with >1 option for the value

    for (const auto* sw : game.getAllSwitches()) {
        if (normalizeName(sw->name) == wantName) {
            // Whole-switch match: prefer exact value, else normalized value.
            int exact = -1, normMatch = -1, normCount = 0;
            for (size_t i = 0; i < sw->options.size(); i++) {
                if (sw->options[i].name == desiredValue) exact = (int)i;
                if (normalizeValue(sw->options[i].name) == wantVal) {
                    if (normMatch < 0) normMatch = (int)i;
                    normCount++;
                }
            }
            int chosen = exact >= 0 ? exact : normMatch;
            if (chosen >= 0) {
                valueMatches.push_back({sw->name, chosen, sw->options[chosen].name});
                if (exact < 0 && normCount > 1) internallyAmbiguous = true;
            } else {
                for (size_t i = 0; i < sw->options.size(); i++)
                    nameOnly.push_back({sw->name, (int)i, sw->options[i].name});
            }
            continue;
        }

        // Combined packed switch that wasn't decomposed (e.g. a "CREDIT/LEVEL"
        // entry with "OFF/OFF" options): match the concept against one field and
        // pick the combined option that changes only that field.
        auto nameFields = splitSlash(sw->name);
        if (nameFields.size() < 2 || sw->options.empty()) continue;
        const size_t N = nameFields.size();
        bool packed = true;
        for (const auto& o : sw->options)
            if (splitSlash(o.name).size() != N) { packed = false; break; }
        if (!packed) continue;

        int cur = std::min<int>(std::max(0, sw->currentIndex), (int)sw->options.size() - 1);
        auto curFields = splitSlash(sw->options[cur].name);
        for (size_t f = 0; f < N; f++) {
            if (normalizeName(nameFields[f]) != wantName) continue;
            int exact = -1, norm = -1;
            for (size_t k = 0; k < sw->options.size(); k++) {
                auto of = splitSlash(sw->options[k].name);
                bool othersOk = true;
                for (size_t j = 0; j < N; j++)
                    if (j != f && of[j] != curFields[j]) { othersOk = false; break; }
                if (!othersOk) continue;
                if (of[f] == desiredValue) exact = (int)k;
                if (normalizeValue(of[f]) == wantVal && norm < 0) norm = (int)k;
            }
            int chosen = exact >= 0 ? exact : norm;
            if (chosen >= 0)
                valueMatches.push_back({sw->name, chosen, sw->options[chosen].name});
            else
                for (size_t k = 0; k < sw->options.size(); k++)
                    nameOnly.push_back({sw->name, (int)k, sw->options[k].name});
        }
    }

    if (!valueMatches.empty()) {
        // A single value-compatible switch is the clear winner even if other
        // same-named switches exist that can't take the value (e.g. 3countb's
        // difficulty "LEVEL" vs the "LEVEL DISP." display toggle).
        result.candidates = valueMatches;
        result.kind = (valueMatches.size() == 1 && !internallyAmbiguous)
            ? CloneMatch::Kind::Confident
            : CloneMatch::Kind::Ambiguous;
    } else if (!nameOnly.empty()) {
        result.candidates = nameOnly;  // name matched but value didn't — confirm
        result.kind = CloneMatch::Kind::Ambiguous;
    } else {
        result.kind = CloneMatch::Kind::NotFound;
    }
    return result;
}

const char* AuditResult::statusText() const {
    switch (status) {
        case Status::Ok:                  return "OK (matches P-ROM)";
        case Status::StructureMismatch:   return "MISMATCH (stale or wrong ROM)";
        case Status::NoSoftDips:          return "no .softdips";
        case Status::NoProgramRom:        return "no program ROM to verify against";
        case Status::RomExtractFailed:    return "ROM present but no softdips table";
        case Status::SoftDipsParseFailed: return "unparseable .softdips";
    }
    return "unknown";
}

// SoftDipsFile implementation
std::vector<DipSwitch*> SoftDipsFile::getAllSwitches() {
    std::vector<DipSwitch*> result;
    for (auto& section : sections) {
        for (auto& sw : section.switches) result.push_back(&sw);
    }
    return result;
}

std::vector<const DipSwitch*> SoftDipsFile::getAllSwitches() const {
    std::vector<const DipSwitch*> result;
    for (const auto& section : sections) {
        for (const auto& sw : section.switches) result.push_back(&sw);
    }
    return result;
}

DipSwitch* SoftDipsFile::findSwitch(const std::string& name) {
    for (auto& section : sections) {
        for (auto& sw : section.switches) {
            if (sw.name == name) return &sw;
        }
    }
    return nullptr;
}

const DipSwitch* SoftDipsFile::findSwitch(const std::string& name) const {
    for (const auto& section : sections) {
        for (const auto& sw : section.switches) {
            if (sw.name == name) return &sw;
        }
    }
    return nullptr;
}

} // namespace softdips