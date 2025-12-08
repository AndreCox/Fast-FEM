#pragma once
#include <fstream>
#include <cstdint>

// File magic (4 bytes) followed by a format version number (uint32_t)
constexpr std::uint32_t FILE_MAGIC = 0x53595356; // "SYSV" magic number
constexpr std::uint32_t FILE_FORMAT_VERSION = 2; // bumped format for unit metadata

void writeString(std::ofstream &ofs, const std::string &str);
std::string readString(std::ifstream &ifs);
