#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

struct PackShader {
    bool isVs;
    uint32_t offset;    // of the microcode in the unpacked data, big-endian as on the console
    uint32_t words;
};

// Unpacks a bundle into data and lists the shader containers in it. False if the file is missing or doesn't parse.
bool shaderpack_load(const std::string& path, std::vector<uint8_t>& data, std::vector<PackShader>& out);
