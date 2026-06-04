// Self-contained unit tests for the softdips core. No external ROM files are
// needed — every fixture is built in memory — so these run identically on all
// CI platforms. Returns non-zero if any check fails (CTest treats that as fail).

#include "softdips.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace softdips;

// ── Tiny assertion framework ────────────────────────────────────────────────
static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                           \
    do {                                                                      \
        ++g_checks;                                                           \
        if (!(cond)) {                                                        \
            ++g_failures;                                                     \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__               \
                      << ": " #cond "\n";                                     \
        }                                                                     \
    } while (0)

template <typename A, typename B>
static void checkEq(const A& a, const B& b, const char* expr, const char* file, int line) {
    ++g_checks;
    if (!(a == b)) {
        ++g_failures;
        std::cerr << "FAIL " << file << ":" << line << ": " << expr
                  << "  (" << a << " != " << b << ")\n";
    }
}
#define CHECK_EQ(a, b) checkEq((a), (b), #a " == " #b, __FILE__, __LINE__)

// ── Fixture helpers ─────────────────────────────────────────────────────────

static void appendField(std::vector<uint8_t>& v, const std::string& s) {
    std::string f = s;
    f.resize(12, ' ');
    v.insert(v.end(), f.begin(), f.end());
}

// Build the on-disk (byte-swapped) bytes of a .softdips file from its logical
// big-endian components, ready to hand to SoftDipsParser::parse.
static std::vector<uint8_t> buildFile(const std::string& title,
                                      const uint8_t special[6],
                                      const uint8_t header[10],
                                      const std::vector<std::string>& text) {
    std::vector<uint8_t> v;
    std::string t = title;
    t.resize(16, ' ');
    v.insert(v.end(), t.begin(), t.end());
    v.insert(v.end(), special, special + 6);
    v.insert(v.end(), header, header + 10);
    for (const auto& f : text) appendField(v, f);
    SoftDipsParser::byteSwap16(v.data(), v.size());  // -> on-disk form
    return v;
}

// Return the logical (big-endian) special-list bytes that `file` would write.
static std::vector<uint8_t> writtenSpecial(const SoftDipsFile& file) {
    auto bytes = SoftDipsParser::toBytes(file);          // on-disk (swapped)
    SoftDipsParser::byteSwap16(bytes.data(), bytes.size());  // -> logical
    return std::vector<uint8_t>(bytes.begin() + 16, bytes.begin() + 22);
}

// ── Tests ───────────────────────────────────────────────────────────────────

// Wiki "TEST ROM" example: LIVES (4 choices, default #2) + HOW TO PLAY (2).
static void test_parse_simple() {
    uint8_t special[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t header[10]  = {0x24, 0x02, 0, 0, 0, 0, 0, 0, 0, 0};
    auto bytes = buildFile("TEST ROM", special, header,
                           {"LIVES", "1", "2", "3", "4", "HOW TO PLAY", "WITH", "WITHOUT"});
    auto f = SoftDipsParser::parse(bytes);
    CHECK(f.has_value());
    CHECK_EQ(f->gameName, std::string("TEST ROM"));
    auto sw = f->getAllSwitches();
    CHECK_EQ(sw.size(), size_t(2));
    if (sw.size() == 2) {
        CHECK_EQ(sw[0]->name, std::string("LIVES"));
        CHECK_EQ(sw[0]->options.size(), size_t(4));
        CHECK_EQ(sw[0]->currentIndex, 2);                 // high nibble of 0x24
        CHECK_EQ(sw[1]->name, std::string("HOW TO PLAY"));
        CHECK_EQ(sw[1]->options.size(), size_t(2));
    }
}

// parse -> toBytes -> parse must preserve names/options/selections.
static void test_roundtrip() {
    uint8_t special[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t header[10]  = {0x24, 0x02, 0, 0, 0, 0, 0, 0, 0, 0};
    auto bytes = buildFile("TEST ROM", special, header,
                           {"LIVES", "1", "2", "3", "4", "HOW TO PLAY", "WITH", "WITHOUT"});
    auto a = SoftDipsParser::parse(bytes);
    auto b = SoftDipsParser::parse(SoftDipsParser::toBytes(*a));
    CHECK(b.has_value());
    auto sa = a->getAllSwitches();
    auto sb = b->getAllSwitches();
    CHECK_EQ(sa.size(), sb.size());
    for (size_t i = 0; i < sa.size() && i < sb.size(); i++) {
        CHECK_EQ(sa[i]->name, sb[i]->name);
        CHECK_EQ(sa[i]->currentIndex, sb[i]->currentIndex);
        CHECK_EQ(sa[i]->options.size(), sb[i]->options.size());
    }
}

// Editing a List setting flips exactly one metadata byte's high nibble.
static void test_edit_one_byte() {
    uint8_t special[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t header[10]  = {0x24, 0x02, 0, 0, 0, 0, 0, 0, 0, 0};
    auto f = SoftDipsParser::parse(buildFile("TEST ROM", special, header,
        {"LIVES", "1", "2", "3", "4", "HOW TO PLAY", "WITH", "WITHOUT"}));
    auto before = SoftDipsParser::toBytes(*f);
    f->getAllSwitches()[0]->currentIndex = 0;  // LIVES -> 1
    auto after = SoftDipsParser::toBytes(*f);
    int diff = 0;
    for (size_t i = 0; i < before.size(); i++) diff += (before[i] != after[i]);
    CHECK_EQ(diff, 1);
}

// Count2 (continues): 0=WITHOUT, 1-99, 100=INFINITE. Value round-trips as a full byte.
static void test_count2_infinite() {
    uint8_t special[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x64};  // count2 = 100
    uint8_t header[10]  = {0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    auto f = SoftDipsParser::parse(buildFile("GAME", special, header,
        {"CONTINUE", "DEMO SOUND", "ON", "OFF"}));
    auto sw = f->getAllSwitches();
    CHECK(sw.size() >= 1);
    CHECK(sw[0]->kind == DipSwitch::Kind::Count2);
    CHECK_EQ(sw[0]->options.size(), size_t(101));        // WITHOUT,1..99,INFINITE
    CHECK_EQ(sw[0]->currentIndex, 100);
    CHECK_EQ(sw[0]->options[100].name, std::string("INFINITE"));
    CHECK_EQ(sw[0]->options[0].name, std::string("WITHOUT"));
    CHECK_EQ((int)writtenSpecial(*f)[5], 0x64);          // not nibble-mangled
}

// Count1 (e.g. lives): full-byte value, 1-99.
static void test_count1_value() {
    uint8_t special[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0x3C, 0xFF};  // count1 = 60
    uint8_t header[10]  = {0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    auto f = SoftDipsParser::parse(buildFile("GAME", special, header,
        {"PLAY TIME", "DEMO SOUND", "ON", "OFF"}));
    auto sw = f->getAllSwitches();
    CHECK(sw[0]->kind == DipSwitch::Kind::Count1);
    CHECK_EQ(sw[0]->options[sw[0]->currentIndex].name, std::string("60"));
    CHECK_EQ((int)writtenSpecial(*f)[4], 0x3C);
}

// Time settings split into MIN/SEC with full-second granularity and BCD round-trip.
static void test_time_split() {
    uint8_t special[6] = {0x03, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};  // time1 = 3:00 (BCD)
    uint8_t header[10]  = {0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    auto f = SoftDipsParser::parse(buildFile("GAME", special, header,
        {"PLAY TIME", "DEMO SOUND", "ON", "OFF"}));
    auto sw = f->getAllSwitches();
    CHECK(sw.size() >= 2);
    CHECK(sw[0]->kind == DipSwitch::Kind::Time && sw[0]->timeField == 0);
    CHECK(sw[1]->kind == DipSwitch::Kind::Time && sw[1]->timeField == 1);
    CHECK_EQ(sw[0]->options[sw[0]->currentIndex].name, std::string("3"));   // min
    CHECK_EQ(sw[1]->options[sw[1]->currentIndex].name, std::string("0"));   // sec
    CHECK_EQ(sw[1]->options.size(), size_t(60));                            // full seconds
    // set 1:05 and confirm BCD word
    sw[0]->currentIndex = 1;
    sw[1]->currentIndex = 5;
    auto special2 = writtenSpecial(*f);
    CHECK_EQ((int)special2[0], 0x01);
    CHECK_EQ((int)special2[1], 0x05);
}

// Packed "CREDIT/LEVEL" decomposes into two sub-switches and re-packs losslessly.
static void test_decompose_repack() {
    uint8_t special[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t header[10]  = {0x04, 0, 0, 0, 0, 0, 0, 0, 0, 0};  // 4 options
    auto f = SoftDipsParser::parse(buildFile("GAME", special, header,
        {"CREDIT/LEVEL", "OFF/OFF", "OFF/ON", "ON/OFF", "ON/ON"}));
    auto sw = f->getAllSwitches();
    CHECK_EQ(sw.size(), size_t(2));
    CHECK_EQ(sw[0]->name, std::string("CREDIT"));
    CHECK_EQ(sw[1]->name, std::string("LEVEL"));
    CHECK(sw[0]->packGroup >= 0 && sw[1]->packGroup == sw[0]->packGroup);
    // Set CREDIT=ON (idx1), LEVEL=OFF (idx0) -> combined "ON/OFF" = option index 2.
    sw[0]->currentIndex = 1;
    sw[1]->currentIndex = 0;
    auto g = SoftDipsParser::parse(SoftDipsParser::toBytes(*f));
    auto gs = g->getAllSwitches();
    CHECK_EQ(gs[0]->options[gs[0]->currentIndex].name, std::string("ON"));
    CHECK_EQ(gs[1]->options[gs[1]->currentIndex].name, std::string("OFF"));
}

static void test_normalize() {
    CHECK_EQ(SoftDipsParser::normalizeName("CREDIT DISP."), std::string("CREDIT"));
    CHECK_EQ(SoftDipsParser::normalizeName("DISP CREDIT"),  std::string("CREDIT"));
    CHECK_EQ(SoftDipsParser::normalizeName("LVL DISPLAY"),  std::string("LEVEL"));
    CHECK_EQ(SoftDipsParser::normalizeName("DSP/CREDIT"),   std::string("CREDIT"));
    CHECK_EQ(SoftDipsParser::normalizeName("INSTRUCTION"),  std::string("HOW TO PLAY"));
    CHECK_EQ(SoftDipsParser::normalizeName("PLAY MANUAL"),  std::string("HOW TO PLAY"));
    CHECK_EQ(SoftDipsParser::normalizeValue("WITHOUT"),  std::string("DISABLE"));
    CHECK_EQ(SoftDipsParser::normalizeValue("OFF"),      std::string("DISABLE"));
    CHECK_EQ(SoftDipsParser::normalizeValue("NO"),       std::string("DISABLE"));
    CHECK_EQ(SoftDipsParser::normalizeValue("WITH OUT"), std::string("DISABLE"));
    CHECK_EQ(SoftDipsParser::normalizeValue("ON"),       std::string("ENABLE"));
    CHECK_EQ(SoftDipsParser::normalizeValue("WITH"),     std::string("ENABLE"));
}

// Build a one-switch file directly for matcher tests.
static SoftDipsFile makeGame(const std::string& name,
                             const std::vector<std::string>& opts, int cur) {
    SoftDipsFile f;
    DipSwitchSection sec;
    DipSwitch sw;
    sw.name = name;
    for (const auto& o : opts) sw.options.push_back({o});
    sw.currentIndex = cur;
    sec.switches.push_back(sw);
    f.sections.push_back(sec);
    return f;
}

static void test_match_crossgame() {
    // "CREDIT" disable resolves across naming/value variants.
    auto g1 = makeGame("CREDIT DISP.", {"WITH", "WITHOUT"}, 0);
    auto m1 = SoftDipsParser::matchSetting(g1, "CREDIT", "WITHOUT");
    CHECK(m1.kind == CloneMatch::Kind::Confident);
    CHECK(!m1.candidates.empty());
    if (!m1.candidates.empty()) CHECK_EQ(m1.candidates[0].optionName, std::string("WITHOUT"));

    auto g2 = makeGame("DISP CREDIT", {"ON", "OFF"}, 0);
    auto m2 = SoftDipsParser::matchSetting(g2, "CREDIT", "WITHOUT");
    CHECK(m2.kind == CloneMatch::Kind::Confident);
    if (!m2.candidates.empty()) CHECK_EQ(m2.candidates[0].optionName, std::string("OFF"));

    auto g3 = makeGame("DIFFICULTY", {"EASY", "HARD"}, 0);
    auto m3 = SoftDipsParser::matchSetting(g3, "CREDIT", "WITHOUT");
    CHECK(m3.kind == CloneMatch::Kind::NotFound);
}

// Helper: is `name` present in a result list?
static bool contains(const std::vector<std::string>& v, const std::string& s) {
    for (const auto& x : v) if (x == s) return true;
    return false;
}

static void test_rank_program_roms() {
    // BackBit extensions (.p1/.pd/.ep1) and MAME -p1.bin are recognised; other
    // ROMs and data files are excluded.
    auto r = SoftDipsParser::rankProgramRoms(
        {"readme.txt", "068-c1.bin", "prom.p1", "068-p1.bin", "cart.pd", "pbobblen.ep1"});
    CHECK(contains(r, "prom.p1"));
    CHECK(contains(r, "cart.pd"));
    CHECK(contains(r, "068-p1.bin"));
    CHECK(contains(r, "pbobblen.ep1"));   // .ep1 first program ROM (pbobblen)
    CHECK(!contains(r, "readme.txt"));
    CHECK(!contains(r, "068-c1.bin"));    // character ROM, not program

    // A lone .ep1 is still found.
    auto only = SoftDipsParser::rankProgramRoms({"pbobblen.ep1"});
    CHECK_EQ(only.size(), size_t(1));
    if (!only.empty()) CHECK_EQ(only[0], std::string("pbobblen.ep1"));

    // BackBit naming ranks ahead of MAME naming for the same bank.
    auto order = SoftDipsParser::rankProgramRoms({"068-p1.bin", "prom.p1"});
    CHECK_EQ(order.size(), size_t(2));
    if (order.size() == 2) CHECK_EQ(order[0], std::string("prom.p1"));

    // MAME program ROMs with region/revision suffixes (p1sp, p1pa, p1pl, p1up,
    // p1sa, p1p, p1bl, p1c, p1k) and other first-bank tokens (pg1, ph1, hp1).
    for (const char* fn : {"abc-p1sp.bin", "abc-p1pa.bin", "abc-p1pl.bin",
                           "abc-p1up.bin", "abc-p1sa.bin", "abc-p1p.bin",
                           "abc-pg1.p1", "abc-ph1.p1", "abc-hp1.p1"}) {
        auto r = SoftDipsParser::rankProgramRoms({fn});
        CHECK_EQ(r.size(), size_t(1));
        if (!r.empty()) CHECK_EQ(r[0], std::string(fn));
    }

    // Character/sound/voice ROMs are not program ROMs.
    auto none = SoftDipsParser::rankProgramRoms(
        {"068-c1.bin", "068-c2.bin", "068-m1.m1", "068-v1.v1", "068-s1.s1"});
    CHECK_EQ(none.size(), size_t(0));

    // The first program ROM (bank 1) is tried before later banks (p2), whatever
    // the naming — the softdips header lives in the first PROM.
    auto banks = SoftDipsParser::rankProgramRoms({"007-p2.p2", "007-p1.p1"});
    CHECK_EQ(banks.size(), size_t(2));
    if (banks.size() == 2) CHECK_EQ(banks[0], std::string("007-p1.p1"));

    // A few sets load p2 first; a lone p2 is still offered as a candidate.
    auto p2first = SoftDipsParser::rankProgramRoms({"xyz-p2.bin"});
    CHECK_EQ(p2first.size(), size_t(1));
    if (!p2first.empty()) CHECK_EQ(p2first[0], std::string("xyz-p2.bin"));
}

int main() {
    test_parse_simple();
    test_roundtrip();
    test_edit_one_byte();
    test_count2_infinite();
    test_count1_value();
    test_time_split();
    test_decompose_repack();
    test_normalize();
    test_match_crossgame();
    test_rank_program_roms();

    std::cout << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    if (g_failures) {
        std::cerr << g_failures << " check(s) FAILED\n";
        return 1;
    }
    std::cout << "All tests passed.\n";
    return 0;
}
