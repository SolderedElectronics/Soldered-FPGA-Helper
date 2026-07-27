// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Gwenhael Goavec-Merou <gwenhael.goavec-merou@trabucayre.com>
 */

#ifndef SRC_JSONPARSER_HPP_
#define SRC_JSONPARSER_HPP_

#include <map>
#include <string>
#include <vector>

/*!
 * \file jsonParser
 * \class JSONParser
 * \brief JSON string Parser
 * \author Gwenhael Goavec-Merou
 */
class JSONParser {
	public:
		JSONParser() = default;

		/* Append vector with a new string to
		 * parse
		 */
		void append(std::string str) { _raw_json.push_back(str); }
		void clear()
		{
			_raw_json.clear();
			_map_json.clear();
		}

		bool parse();

		void display() const;

		std::string get_value_from_key(const std::string &key) const;

	private:
		std::string replace_ff(const std::string& input) const;
		bool skipSpaces(const std::string& s, size_t& i) const;
		bool parseString(const std::string& s, size_t& i, std::string& out) const;
		bool parseValue(const std::string& s, size_t& i,
				const std::string& prefix, const std::string& key);
		bool parseObject(const std::string& s, size_t& i,
				const std::string& prefix = "");

		std::vector<std::string> _raw_json;
		std::map<std::string, std::string> _map_json;
};
#endif  // SRC_JSONPARSER_HPP_
