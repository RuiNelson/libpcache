#pragma once

#include <cstdint>
#include <string>
#include <vector>

std::vector<uint8_t> read_file(const std::string &path);
bool write_file(const std::string &path, const uint8_t *data, size_t size);
std::vector<uint8_t> read_id_file(const std::string &path);