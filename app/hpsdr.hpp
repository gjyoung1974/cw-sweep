#pragma once
// Receive-only client for openHPSDR "protocol 1" radios on the network:
// Hermes Lite 2, Radioberry (which emulates a Hermes Lite), Hermes, ...

#include <memory>
#include <string>

#include "sources.hpp"

namespace cwapp {

// `target` is empty (find the radio by broadcast), ADDRESS or ADDRESS:PORT.
std::unique_ptr<Source> openHpsdr(const SourceOptions& o, const std::string& target, std::string& err);

// Prints the radios that answer a discovery broadcast. False if there are none.
bool listHpsdr();

}  // namespace cwapp
