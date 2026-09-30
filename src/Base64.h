#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

// Decodes standard or URL-safe base64. Whitespace and an optional
// "data:...;base64," prefix are ignored. Returns nullopt on malformed input.
std::optional<std::vector<uint8_t>> base64Decode(std::string_view in);
