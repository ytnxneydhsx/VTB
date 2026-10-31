#include "vtb/config/config_file_reader.hpp"

#include <fstream>
#include <iterator>
#include <stdexcept>

namespace vtb {

std::string ConfigurationFileReader::read(const std::string& path) const {
    if (path.empty()) {
        throw std::invalid_argument("Configuration file path is empty");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Unable to open configuration file: " + path);
    }
    std::string content{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    if (input.bad()) {
        throw std::runtime_error("Unable to read configuration file: " + path);
    }
    if (content.empty()) {
        throw std::runtime_error("Configuration file is empty: " + path);
    }
    return content;
}

}  // namespace vtb
