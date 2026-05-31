#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include "softdips.h"

void printUsage(const char* programName) {
    std::cout << "Usage: " << programName << " [options] <file.softdips>" << std::endl;
    std::cout << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  -h, --help           Show this help message" << std::endl;
    std::cout << "  -l, --list           List all dip switches" << std::endl;
    std::cout << "  -g, --get <name>     Get value of a dip switch" << std::endl;
    std::cout << "  -s, --set <name> <value>  Set value of a dip switch" << std::endl;
    std::cout << "  -d, --dump           Dump raw hex data" << std::endl;
    std::cout << "  -a, --audit <dir>    Audit .softdips file(s) against the P-ROM table" << std::endl;
    std::cout << "                       (give a game dir or a parent of game dirs)" << std::endl;
    std::cout << std::endl;
    std::cout << "Examples:" << std::endl;
    std::cout << "  " << programName << " -l game.softdips" << std::endl;
    std::cout << "  " << programName << " -g DIFFICULTY game.softdips" << std::endl;
    std::cout << "  " << programName << " -s DIFFICULTY \"LEVEL 3\" game.softdips" << std::endl;
    std::cout << "  " << programName << " --audit /path/to/NeoGeo" << std::endl;
}

// Returns the number of problem (mismatch/unparseable) games found.
int auditPath(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(path, ec)) {
        std::cerr << "Error: --audit expects a directory" << std::endl;
        return -1;
    }

    // A single game dir if it directly holds a .softdips or a program ROM;
    // otherwise audit each immediate subdirectory.
    std::vector<fs::path> gameDirs;
    bool selfIsGame = fs::exists(fs::path(path) / ".softdips", ec) ||
                      !softdips::SoftDipsParser::findProgramRoms(path).empty();
    if (selfIsGame) {
        gameDirs.push_back(path);
    } else {
        for (const auto& e : fs::directory_iterator(path, ec))
            if (e.is_directory(ec)) gameDirs.push_back(e.path());
        std::sort(gameDirs.begin(), gameDirs.end());
    }

    int ok = 0, problems = 0, skipped = 0;
    for (const auto& dir : gameDirs) {
        auto r = softdips::SoftDipsParser::auditGameDir(dir);
        // Skip directories that are neither games nor have softdips.
        if (r.status == softdips::AuditResult::Status::NoSoftDips && r.gameName.empty())
            continue;

        std::string label = dir.filename().string();
        std::cout << (r.isProblem() ? "✗ " : (r.status == softdips::AuditResult::Status::Ok ? "✓ " : "· "))
                  << label;
        if (!r.gameName.empty()) std::cout << "  [" << r.gameName << "]";
        std::cout << "  — " << r.statusText() << std::endl;
        for (const auto& d : r.differences) std::cout << "      - " << d << std::endl;

        if (r.isProblem()) problems++;
        else if (r.status == softdips::AuditResult::Status::Ok) ok++;
        else skipped++;
    }

    std::cout << "\n" << ok << " OK, " << problems << " problem(s), "
              << skipped << " not verified" << std::endl;
    return problems;
}

void listSwitches(const softdips::SoftDipsFile& file) {
    std::cout << "Game: " << file.gameName << std::endl;
    std::cout << std::endl;
    
    auto switches = file.getAllSwitches();
    for (const auto* sw : switches) {
        std::cout << sw->name << ":" << std::endl;
        for (size_t i = 0; i < sw->options.size(); i++) {
            std::string marker = (i == static_cast<size_t>(sw->currentIndex)) ? " *" : "";
            std::cout << "  " << i << ": " << sw->options[i].name << marker << std::endl;
        }
        std::cout << std::endl;
    }
}

void getSwitch(const softdips::SoftDipsFile& file, const std::string& name) {
    const auto* sw = file.findSwitch(name);
    if (!sw) {
        std::cerr << "Error: Dip switch '" << name << "' not found" << std::endl;
        return;
    }
    
    std::cout << sw->name << " = " << sw->options[sw->currentIndex].name << std::endl;
}

void setSwitch(softdips::SoftDipsFile& file, const std::string& name, const std::string& value) {
    auto* sw = file.findSwitch(name);
    if (!sw) {
        std::cerr << "Error: Dip switch '" << name << "' not found" << std::endl;
        return;
    }
    
    // Find the option index
    for (size_t i = 0; i < sw->options.size(); i++) {
        if (sw->options[i].name == value) {
            sw->currentIndex = static_cast<int>(i);
            std::cout << "Set " << sw->name << " = " << value << std::endl;
            return;
        }
    }
    
    std::cerr << "Error: Option '" << value << "' not found for dip switch '" << name << "'" << std::endl;
    std::cerr << "Available options:" << std::endl;
    for (const auto& opt : sw->options) {
        std::cerr << "  " << opt.name << std::endl;
    }
}

void dumpRaw(const softdips::SoftDipsFile& file) {
    auto bytes = softdips::SoftDipsParser::toBytes(file);
    
    std::cout << "Game: " << file.gameName << std::endl;
    std::cout << "Header marker: ";
    for (int i = 0; i < 6; i++) {
        printf("%02X ", file.headerMarker[i]);
    }
    std::cout << std::endl;
    
    std::cout << "Settings data: ";
    for (int i = 0; i < 10; i++) {
        printf("%02X ", file.settingsData[i]);
    }
    std::cout << std::endl;
    
    std::cout << "Total size: " << bytes.size() << " bytes" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }
    
    std::string filePath;
    std::string command;
    std::string arg1;
    std::string arg2;
    
    // Parse arguments. Uses an explicit index since some options consume the
    // following token(s) as their values.
    int i = 1;
    while (i < argc) {
        std::string arg = argv[i++];

        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "-l" || arg == "--list") {
            command = "list";
        } else if (arg == "-g" || arg == "--get") {
            command = "get";
            if (i < argc) {
                arg1 = argv[i++];
            }
        } else if (arg == "-s" || arg == "--set") {
            command = "set";
            if (i < argc) {
                arg1 = argv[i++];
            }
            if (i < argc) {
                arg2 = argv[i++];
            }
        } else if (arg == "-d" || arg == "--dump") {
            command = "dump";
        } else if (arg == "-a" || arg == "--audit") {
            command = "audit";
        } else if (arg[0] != '-') {
            filePath = arg;
        }
    }

    if (filePath.empty()) {
        std::cerr << "Error: No path specified" << std::endl;
        printUsage(argv[0]);
        return 1;
    }

    // Audit operates on directories, not a single parsed .softdips file.
    if (command == "audit") {
        return auditPath(filePath) > 0 ? 2 : 0;
    }

    // Parse the file
    auto file = softdips::SoftDipsParser::parse(filePath);
    if (!file) {
        std::cerr << "Error: Failed to parse file '" << filePath << "'" << std::endl;
        return 1;
    }
    
    // Execute command
    if (command == "list" || command.empty()) {
        listSwitches(*file);
    } else if (command == "get") {
        if (arg1.empty()) {
            std::cerr << "Error: No dip switch name specified" << std::endl;
            return 1;
        }
        getSwitch(*file, arg1);
    } else if (command == "set") {
        if (arg1.empty() || arg2.empty()) {
            std::cerr << "Error: Dip switch name and value required" << std::endl;
            return 1;
        }
        setSwitch(*file, arg1, arg2);
        
        // Save the file
        if (!softdips::SoftDipsParser::write(filePath, *file)) {
            std::cerr << "Error: Failed to write file '" << filePath << "'" << std::endl;
            return 1;
        }
    } else if (command == "dump") {
        dumpRaw(*file);
    } else {
        std::cerr << "Error: Unknown command '" << command << "'" << std::endl;
        printUsage(argv[0]);
        return 1;
    }
    
    return 0;
}