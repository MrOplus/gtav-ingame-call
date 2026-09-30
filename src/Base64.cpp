#include "Base64.h"

static int b64Value(char c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+' || c == '-') return 62;
	if (c == '/' || c == '_') return 63;
	return -1;
}

std::optional<std::vector<uint8_t>> base64Decode(std::string_view in)
{
	if (in.substr(0, 5) == "data:")
	{
		auto comma = in.find(',');
		if (comma == std::string_view::npos)
			return std::nullopt;
		in.remove_prefix(comma + 1);
	}

	std::vector<uint8_t> out;
	out.reserve(in.size() * 3 / 4);
	uint32_t acc = 0;
	int bits = 0;
	for (char c : in)
	{
		if (c == '=')
			break;
		if (c == ' ' || c == '\n' || c == '\r' || c == '\t')
			continue;
		int v = b64Value(c);
		if (v < 0)
			return std::nullopt;
		acc = (acc << 6) | static_cast<uint32_t>(v);
		bits += 6;
		if (bits >= 8)
		{
			bits -= 8;
			out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
		}
	}
	return out;
}
