// Emscripten/Embind bindings


#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <string>
#include <vector>

#include "softdips.h"

namespace {

void jsonEscape(const std::string& s, std::string& out) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(hex[(c >> 4) & 0xF]);
                    out.push_back(hex[c & 0xF]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

const char* kindName(softdips::DipSwitch::Kind k) {
    switch (k) {
        case softdips::DipSwitch::Kind::List:   return "list";
        case softdips::DipSwitch::Kind::Time:   return "time";
        case softdips::DipSwitch::Kind::Count1: return "count1";
        case softdips::DipSwitch::Kind::Count2: return "count2";
    }
    return "list";
}

// Copy a byte vector out of the wasm heap into a fresh JS Uint8Array.
emscripten::val toUint8Array(const std::vector<uint8_t>& data) {
    return emscripten::val(emscripten::typed_memory_view(data.size(), data.data()))
        .call<emscripten::val>("slice");
}

void appendCloneMatch(const softdips::CloneMatch& m, std::string& out) {
    const char* kind = m.kind == softdips::CloneMatch::Kind::Confident ? "confident"
                     : m.kind == softdips::CloneMatch::Kind::Ambiguous ? "ambiguous"
                                                                       : "notfound";
    out += "{\"kind\":\"";
    out += kind;
    out += "\",\"candidates\":[";
    for (size_t i = 0; i < m.candidates.size(); ++i) {
        if (i) out.push_back(',');
        const auto& c = m.candidates[i];
        out += "{\"switchName\":";
        jsonEscape(c.switchName, out);
        out += ",\"optionIndex\":" + std::to_string(c.optionIndex);
        out += ",\"optionName\":";
        jsonEscape(c.optionName, out);
        out += "}";
    }
    out += "]}";
}

}  // namespace

// JS holds one handle per open file and mutates the dipswitch selections 
// (by flattened index, or by name for clone), then asks for the bytes back. 
// call .delete() on the JS side when done 
class Document {
public:
    explicit Document(emscripten::val bytes) {
        std::vector<uint8_t> data = emscripten::vecFromJSArray<uint8_t>(bytes);
        auto parsed = softdips::SoftDipsParser::parse(data);
        if (parsed) {
            file_ = std::move(*parsed);
            valid_ = true;
        }
    }

    bool valid() const { return valid_; }

    std::string gameName() const { return valid_ ? file_.gameName : std::string(); }

    // JSON: { "gameName", "switches": [ { "index","name","kind","currentIndex",
    //   "defaultIndex","timeField","metaByteIndex","options":[...] } ] }
    std::string json() const {
        if (!valid_) return "{\"gameName\":\"\",\"switches\":[]}";

        std::string out = "{\"gameName\":";
        jsonEscape(file_.gameName, out);
        out += ",\"switches\":[";

        auto switches = file_.getAllSwitches();
        for (size_t i = 0; i < switches.size(); ++i) {
            const auto* sw = switches[i];
            if (i) out.push_back(',');
            out += "{\"index\":" + std::to_string(i);
            out += ",\"name\":";
            jsonEscape(sw->name, out);
            out += ",\"kind\":\"";
            out += kindName(sw->kind);
            out += "\",\"currentIndex\":" + std::to_string(sw->currentIndex);
            out += ",\"defaultIndex\":" + std::to_string(sw->defaultIndex);
            out += ",\"timeField\":" + std::to_string(sw->timeField);
            out += ",\"metaByteIndex\":" + std::to_string(sw->metaByteIndex);
            out += ",\"options\":[";
            for (size_t o = 0; o < sw->options.size(); ++o) {
                if (o) out.push_back(',');
                jsonEscape(sw->options[o].name, out);
            }
            out += "]}";
        }
        out += "]}";
        return out;
    }

    // Set the selected option of the switch at flattened index `switchIndex`.
    bool setCurrent(int switchIndex, int optionIndex) {
        if (!valid_) return false;
        auto switches = file_.getAllSwitches();
        if (switchIndex < 0 || switchIndex >= static_cast<int>(switches.size()))
            return false;
        return applyOption(switches[switchIndex], optionIndex);
    }

    // Set the selected option of the first switch named `switchName` (used by
    // clone, which resolves matches to switch names).
    bool setByName(const std::string& switchName, int optionIndex) {
        if (!valid_) return false;
        return applyOption(file_.findSwitch(switchName), optionIndex);
    }

    void resetDefaults() {
        if (!valid_) return;
        for (auto* sw : file_.getAllSwitches()) sw->currentIndex = sw->defaultIndex;
    }

    // Resolve a clone setting (a concept name + desired value, taken from a
    // source game's switch) against this game. Returns the CloneMatch as JSON.
    std::string matchSetting(const std::string& conceptName,
                             const std::string& desiredValue) const {
        if (!valid_) return "{\"kind\":\"notfound\",\"candidates\":[]}";
        auto m = softdips::SoftDipsParser::matchSetting(file_, conceptName, desiredValue);
        std::string out;
        appendCloneMatch(m, out);
        return out;
    }

    // Serialize back to .softdips bytes as a (copied) Uint8Array.
    emscripten::val toBytes() const {
        std::vector<uint8_t> data =
            valid_ ? softdips::SoftDipsParser::toBytes(file_) : std::vector<uint8_t>{};
        return toUint8Array(data);
    }

private:
    bool applyOption(softdips::DipSwitch* sw, int optionIndex) {
        if (!sw || optionIndex < 0 || optionIndex >= static_cast<int>(sw->options.size()))
            return false;
        sw->currentIndex = optionIndex;
        return true;
    }

    softdips::SoftDipsFile file_;
    bool valid_ = false;
};

// ── Free functions: directory-level primitives driven from JS ────────────────

// Rank the program-ROM candidates among a directory's filenames.
emscripten::val rankProgramRoms(emscripten::val filenames) {
    std::vector<std::string> names = emscripten::vecFromJSArray<std::string>(filenames);
    emscripten::val arr = emscripten::val::array();
    for (const auto& n : softdips::SoftDipsParser::rankProgramRoms(names))
        arr.call<void>("push", emscripten::val(n));
    return arr;
}

// Extract the softdips table from raw program-ROM bytes. Returns
// { found, diag, gameName, bytes } where bytes is the generated .softdips
// (empty Uint8Array when nothing was found).
emscripten::val extractSoftdips(emscripten::val romBytes) {
    std::vector<uint8_t> data = emscripten::vecFromJSArray<uint8_t>(romBytes);
    std::string diag;
    auto extracted = softdips::SoftDipsParser::extractFromRom(data, &diag);

    emscripten::val out = emscripten::val::object();
    if (extracted && !extracted->sections.empty()) {
        out.set("found", true);
        out.set("diag", diag);
        out.set("gameName", extracted->gameName);
        out.set("bytes", toUint8Array(softdips::SoftDipsParser::toBytes(*extracted)));
    } else {
        out.set("found", false);
        out.set("diag", diag.empty() ? std::string("No soft DIP settings found") : diag);
        out.set("gameName", std::string());
        out.set("bytes", toUint8Array({}));
    }
    return out;
}

// Compare a .softdips file against an authoritative table (both as .softdips
// bytes). Returns { ok, diffs:[...] }; diffs empty == identical structure.
emscripten::val compareStructure(emscripten::val softdipsBytes, emscripten::val romTableBytes) {
    std::vector<uint8_t> a = emscripten::vecFromJSArray<uint8_t>(softdipsBytes);
    std::vector<uint8_t> b = emscripten::vecFromJSArray<uint8_t>(romTableBytes);
    auto soft = softdips::SoftDipsParser::parse(a);
    auto rom  = softdips::SoftDipsParser::parse(b);

    emscripten::val out = emscripten::val::object();
    emscripten::val diffs = emscripten::val::array();
    if (!soft || !rom) {
        out.set("ok", false);
        out.set("parseFailed", true);
        out.set("diffs", diffs);
        return out;
    }
    auto d = softdips::SoftDipsParser::compareStructure(*soft, *rom);
    for (const auto& s : d) diffs.call<void>("push", emscripten::val(s));
    out.set("ok", d.empty());
    out.set("parseFailed", false);
    out.set("diffs", diffs);
    return out;
}

EMSCRIPTEN_BINDINGS(softdips_module) {
    emscripten::class_<Document>("Document")
        .constructor<emscripten::val>()
        .function("valid", &Document::valid)
        .function("gameName", &Document::gameName)
        .function("json", &Document::json)
        .function("setCurrent", &Document::setCurrent)
        .function("setByName", &Document::setByName)
        .function("resetDefaults", &Document::resetDefaults)
        .function("matchSetting", &Document::matchSetting)
        .function("toBytes", &Document::toBytes);

    emscripten::function("rankProgramRoms", &rankProgramRoms);
    emscripten::function("extractSoftdips", &extractSoftdips);
    emscripten::function("compareStructure", &compareStructure);
}
