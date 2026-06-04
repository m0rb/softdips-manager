#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <filesystem>

namespace softdips {

// Represents a single option for a dip switch
struct DipSwitchOption {
    std::string name;
};

// Represents a single dip switch with its options
struct DipSwitch {
    std::string name;
    std::vector<DipSwitchOption> options;
    int defaultIndex = 0;  // Index of the default option
    int currentIndex = 0;  // Currently selected option

    // Settings from the 6-byte special list are typed (the BIOS renders their
    // value; no option strings are stored in the ROM):
    //   Time   - a BCD MM:SS word.
    //   Count1 - a counter, 1-99.
    //   Count2 - a "number of times": 0="WITHOUT", 1-99, 100="INFINITE".
    // Simple settings from the 10-byte option header are List (stored options).
    enum class Kind { List, Time, Count1, Count2 };
    Kind kind = Kind::List;

    // Index of the controlling byte(s) in the 16-byte metadata block:
    //   List   : 6-15 (option header) — high nibble holds the current index.
    //   Count1 : 4     Count2 : 5     (special list) — full byte is the value.
    //   Time   : 0 or 2 (special list) — a BCD word (MM:SS).
    int metaByteIndex = -1;

    // Some ROMs pack two (or more) logical switches into one entry, e.g.
    // "CREDIT/LEVEL" with options "ON/OFF". Such entries are decomposed into
    // sub-switches that share one metadata byte; on write they are re-combined.
    // packGroup indexes SoftDipsFile::packGroups (-1 = standalone switch).
    int packGroup = -1;
    int packField = 0;   // which "/"-separated field this sub-switch represents

    // A Time setting is split into two switches (minutes + seconds) that share
    // the same BCD word at metaByteIndex. timeField: 0 = minutes, 1 = seconds.
    int timeField = -1;  // -1 = not a time half
};

// Describes one packed switch that was decomposed into sub-switches, so its
// chosen sub-values can be re-combined into a single option index on write.
struct PackGroup {
    int metaByteIndex = -1;
    // combos[k] = the trimmed "/"-separated field values of combined option k,
    // in the original option order (the high nibble selects k).
    std::vector<std::vector<std::string>> combos;
};

// Represents a section of dip switches (separated by 0x13 marker)
struct DipSwitchSection {
    std::vector<DipSwitch> switches;
};

// Main softdips file structure
struct SoftDipsFile {
    std::string gameName;                    // 16 bytes, space-padded
    uint8_t headerMarker[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};  // 6-byte "special list" (numeric settings)
    uint8_t settingsData[10] = {0};          // 10-byte option header (string settings)
    std::vector<DipSwitchSection> sections;  // Dip switch sections
    std::vector<PackGroup> packGroups;       // decomposed packed-switch metadata

    // Raw big-endian text region (everything after the 32-byte header,
    // 0x13 page markers intact). Preserved verbatim so writes round-trip
    // byte-for-byte; only the metadata nibbles are patched on save.
    std::vector<uint8_t> rawText;
    
    // Get all dip switches across all sections
    std::vector<DipSwitch*> getAllSwitches();
    std::vector<const DipSwitch*> getAllSwitches() const;
    
    // Find a dip switch by name
    DipSwitch* findSwitch(const std::string& name);
    const DipSwitch* findSwitch(const std::string& name) const;
};

// Result of auditing a .softdips file against its P-ROM's authoritative table.
struct AuditResult {
    enum class Status {
        Ok,                  // .softdips structure matches the P-ROM table
        StructureMismatch,   // names/options/counts differ — likely stale or wrong ROM
        NoSoftDips,          // no .softdips present to audit
        NoProgramRom,        // no program ROM found to verify against
        RomExtractFailed,    // program ROM found but no softdips table could be read
        SoftDipsParseFailed  // .softdips present but unparseable
    };
    Status status = Status::Ok;
    std::string gameName;                  // from .softdips (or ROM as a fallback)
    std::vector<std::string> differences;  // human-readable structural differences

    bool isProblem() const {
        return status == Status::StructureMismatch ||
               status == Status::SoftDipsParseFailed;
    }
    const char* statusText() const;
};

// Result of resolving a clone setting (a concept name + desired value) against
// one game's switches, allowing for naming/value differences between games.
struct CloneMatch {
    enum class Kind {
        Confident,  // exactly one switch + one option matched — safe to apply
        Ambiguous,  // matched, but the switch and/or option choice needs confirming
        NotFound    // no switch in this game matches the concept
    };
    struct Candidate {
        std::string switchName;
        int optionIndex = -1;
        std::string optionName;
    };
    Kind kind = Kind::NotFound;
    std::vector<Candidate> candidates;  // 1 for Confident; >=1 for Ambiguous; 0 for NotFound
};

// Parser class for .softdips files
class SoftDipsParser {
public:
    // Parse a .softdips file
    static std::optional<SoftDipsFile> parse(const std::filesystem::path& filePath);
    
    // Parse from raw bytes
    static std::optional<SoftDipsFile> parse(const std::vector<uint8_t>& data);
    
    // Write a .softdips file
    static bool write(const std::filesystem::path& filePath, const SoftDipsFile& file);
    
    // Convert to raw bytes
    static std::vector<uint8_t> toBytes(const SoftDipsFile& file);
    
    // Find candidate program ROMs (the first program ROM holds the softdips
    // header) in a game directory, in priority order. Recognises BackBit
    // naming (prom.p1, *.pd, *.ep1) and MAME naming (NGH-p1.bin, name-p1.bin,
    // including revision suffixes like -p1a.bin, and -pg1.bin variants).
    static std::vector<std::filesystem::path> findProgramRoms(
        const std::filesystem::path& dir);

    // Rank program-ROM candidates among a set of filenames (names only, with
    // extensions — no directory traversal), in the same priority order as
    // findProgramRoms. Lets frontends without filesystem access (the web build)
    // enumerate a directory themselves and pick which files to read. Returns the
    // matching filenames, best candidate first.
    static std::vector<std::string> rankProgramRoms(
        const std::vector<std::string>& filenames);

    // Extract softdips from a P-ROM file
    static std::optional<SoftDipsFile> extractFromRom(const std::filesystem::path& romPath);

    // Extract with diagnostics output (optional)
    static std::optional<SoftDipsFile> extractFromRom(
        const std::filesystem::path& romPath, std::string* diagnostics);

    // Extract the softdips table directly from raw program-ROM bytes (the
    // byte-based core of the path overloads above).
    static std::optional<SoftDipsFile> extractFromRom(
        const std::vector<uint8_t>& romBytes, std::string* diagnostics = nullptr);

    // Extract the softdips table from the first usable program ROM in a dir.
    static std::optional<SoftDipsFile> extractFromDir(
        const std::filesystem::path& dir, std::string* diagnostics = nullptr);

    // Compare two parsed files structurally (names + options + counts),
    // ignoring the user's current selections. Empty result = identical shape.
    static std::vector<std::string> compareStructure(
        const SoftDipsFile& softdips, const SoftDipsFile& fromRom);

    // Audit a game directory: its .softdips against its program ROM table.
    static AuditResult auditGameDir(const std::filesystem::path& gameDir);

    // Normalize a switch name for cross-game matching (uppercase, drop
    // punctuation and display-indicator words like DISP/INDI, LVL->LEVEL).
    static std::string normalizeName(const std::string& raw);

    // Normalize an option value to "ENABLE"/"DISABLE" for on-off style settings
    // (ON/WITH/YES vs OFF/WITHOUT/NO, with optional DISP prefix), else a cleaned
    // literal.
    static std::string normalizeValue(const std::string& raw);

    // Resolve a clone setting (concept name + desired value, taken from a
    // source game's switch) against another game's switches.
    static CloneMatch matchSetting(const SoftDipsFile& game,
                                     const std::string& conceptName,
                                     const std::string& desiredValue);
    
    // Helper function to byte-swap 16-bit values
    static void byteSwap16(uint8_t* data, size_t length);
    
    // Helper function to byte-swap a single 16-bit value
    static uint16_t swap16(uint16_t value);
};

} // namespace softdips