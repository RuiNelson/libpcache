#include <fstream>
#include <iostream>
#include <vector>

#include "file_utils.hxx"
#include "repl.hxx"

std::vector<uint8_t> read_file(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return {};

    f.seekg(0, std::ios::end);
    size_t size = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);

    std::vector<uint8_t> buf(size);
    if (!f.read(reinterpret_cast<char *>(buf.data()), size))
        return {};
    return buf;
}

bool write_file(const std::string &path, const uint8_t *data, size_t size) {
    std::ofstream f(path, std::ios::binary);
    if (!f)
        return false;
    f.write(reinterpret_cast<const char *>(data), size);
    return f.good();
}

std::vector<uint8_t> read_id_file(const std::string &path) {
    auto buf = read_file(path);
    if (buf.empty())
        std::cerr << "Failed to read id file: " << path << "\n";
    return buf;
}