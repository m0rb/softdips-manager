#include <iostream>
#include <filesystem>
#include "softdips.h"

void printUsage() {
    std::cout << "Usage: test_softdips <file.softdips>" << std::endl;
}

void printSoftDipsInfo(const softdips::SoftDipsFile& file) {
    std::cout << "=== SoftDips File Info ===" << std::endl;
    std::cout << "Game Name: " << file.gameName << std::endl;
    std::cout << "Header Marker: ";
    for (int i = 0; i < 6; i++) {
        printf("%02X ", file.headerMarker[i]);
    }
    std::cout << std::endl;
    
    std::cout << "Settings Data: ";
    for (int i = 0; i < 10; i++) {
        printf("%02X ", file.settingsData[i]);
    }
    std::cout << std::endl;
    
    std::cout << "Number of Sections: " << file.sections.size() << std::endl;
    
    auto switches = file.getAllSwitches();
    std::cout << "Total Dip Switches: " << switches.size() << std::endl;
    
    std::cout << std::endl;
    
    for (size_t si = 0; si < file.sections.size(); si++) {
        const auto& section = file.sections[si];
        std::cout << "--- Section " << (si + 1) << " ---" << std::endl;
        
        for (const auto& sw : section.switches) {
            std::cout << "  " << sw.name << ":" << std::endl;
            for (size_t i = 0; i < sw.options.size(); i++) {
                std::string marker = (i == static_cast<size_t>(sw.currentIndex)) ? " *" : "";
                std::cout << "    " << i << ": " << sw.options[i].name << marker << std::endl;
            }
        }
        std::cout << std::endl;
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage();
        return 1;
    }
    
    std::string filePath = argv[1];
    
    if (!std::filesystem::exists(filePath)) {
        std::cerr << "Error: File not found: " << filePath << std::endl;
        return 1;
    }
    
    auto file = softdips::SoftDipsParser::parse(filePath);
    if (!file) {
        std::cerr << "Error: Failed to parse file: " << filePath << std::endl;
        return 1;
    }
    
    printSoftDipsInfo(*file);
    
    // Test writing the file back
    std::string outputPath = filePath + ".test";
    if (softdips::SoftDipsParser::write(outputPath, *file)) {
        std::cout << "Test write successful to: " << outputPath << std::endl;
        
        // Read it back and verify
        auto reloaded = softdips::SoftDipsParser::parse(outputPath);
        if (reloaded && reloaded->gameName == file->gameName) {
            std::cout << "Round-trip test passed!" << std::endl;
        } else {
            std::cerr << "Round-trip test failed!" << std::endl;
        }
        
        // Clean up
        std::filesystem::remove(outputPath);
    } else {
        std::cerr << "Test write failed!" << std::endl;
    }
    
    return 0;
}