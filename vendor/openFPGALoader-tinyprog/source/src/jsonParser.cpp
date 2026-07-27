#include <iostream>
#include <string>

#include "display.hpp"

#include "jsonParser.hpp"

std::string JSONParser::replace_ff(const std::string& input) const
{
	std::string out;
	out.reserve(input.size());
	for (const auto c : input)
		out.push_back((static_cast<unsigned char>(c) == 0xff) ? ' ' : c);
	return out;
}

bool JSONParser::parse()
{
	for (const auto& t : _raw_json) {
		std::string sanitized = replace_ff(t);
		size_t pos = 0;
		if (!parseObject(sanitized, pos, ""))
			return false;
	}
	return true;
}

std::string JSONParser::get_value_from_key(const std::string &key) const
{
	auto value = _map_json.find(key);
	if (value == _map_json.end())
		return "";
	return value->second;
}

void JSONParser::display() const
{
#if 0
	printf("\n");
	for (const auto &t: _raw_json)
		printf("%s\n", t.c_str());
	printf("\n");
#endif
	for (const auto& t : _map_json)
		std::cout << t.first << " = " << t.second << '\n';
}

bool JSONParser::skipSpaces(const std::string& s, size_t& i) const
{
	static constexpr const char* kSpaces = " \t\n\r\f\v";
	size_t next = s.find_first_not_of(kSpaces, i);
	i = (next == std::string::npos) ? s.size() : next;
	return i < s.size();
}

bool JSONParser::parseString(const std::string& s, size_t& i, std::string& out) const
{
	out.clear();
	if (i >= s.size()) {
		printError("Unexpected end of input while parsing string");
		return false;
	}

	if (s[i] != '"') {
		printError("Expected '\"' at position " + std::to_string(i));
		return false;
	}

	const size_t begin = i + 1;
	/* search closing quote */
	const size_t end = s.find('"', begin);
	/* not found: error */
	if (end == std::string::npos) {
		printError("Unterminated string at position " + std::to_string(i));
		return false;
	}

	i = end + 1; // skip closing quote
	out = s.substr(begin, end - begin);
	return true;
}

bool JSONParser::parseValue(const std::string& s, size_t& i,
		const std::string& prefix, const std::string& key)
{
	if (i >= s.size()) {
		printError("Unterminated object");
		return false;
	}

	switch (s[i]) {
		case '"': {
			std::string value;
			if (!parseString(s, i, value))
				return false;
			_map_json[prefix + key] = std::move(value);
			return true;
		}
		case '{':
			return parseObject(s, i, prefix + key + ".");
		default:
			printError("Unsupported JSON value at position " + std::to_string(i));
			return false;
	}
}

bool JSONParser::parseObject(const std::string& s, size_t& i,
				 const std::string& prefix)
{
	/* Blank line */
	if (!skipSpaces(s, i))
		return true;

	switch (s[i]) {
		case '\0':
			return true;
		case '{':
			break;
		default:
			printError("Expected '{' at position " + std::to_string(i));
			return false;
	}

	i++; // skip '{'

	while (i < s.size()) {
		if (!skipSpaces(s, i)) {
			printError("Unterminated object");
			return false;
		}

		switch (s[i]) {
			case '}':
				i++;
				return true;
			default:
				break;
		}

		std::string key;
		if (!parseString(s, i, key))
			return false;

		if (!skipSpaces(s, i)) {
			printError("Unterminated object");
			return false;
		}

		if (s[i] != ':') {
			printError("Expected ':' at position " + std::to_string(i));
			return false;
		}
		i++;

		if (!skipSpaces(s, i)) {
			printError("Unterminated object");
			return false;
		}

		if (!parseValue(s, i, prefix, key))
			return false;

		if (!skipSpaces(s, i)) {
			printError("Unterminated object");
			return false;
		}

		switch (s[i]) {
			case ',':
				i++;
				continue;
			case '}':
				i++;
				return true;
			default:
				printError("Expected ',' or '}' at position " + std::to_string(i));
				return false;
		}
	}

	printError("Unterminated object");
	return false;
}
