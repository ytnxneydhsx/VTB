#pragma once

#include <string>

namespace vtb {

class ConfigurationFileReader final {
public:
    std::string read(const std::string& path) const;
};

}  // namespace vtb
