// k4_gotv — the KeyValues 1 reader (see k4_kv1.hpp).

#include "k4_kv1.hpp"

#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace k4 {
namespace {

bool EqualsNoCase(std::string_view left, std::string_view right) {
	if (left.size() != right.size()) {
		return false;
	}
	for (std::size_t index = 0; index < left.size(); ++index) {
		if (std::tolower(static_cast<unsigned char>(left[index]))
			!= std::tolower(static_cast<unsigned char>(right[index]))) {
			return false;
		}
	}
	return true;
}

class Parser {
public:
	explicit Parser(std::string_view text) : m_text(text) {}

	bool Parse(Kv1Node& root) {
		while (true) {
			SkipSpaceAndComments();
			if (AtEnd()) {
				break;
			}
			Kv1Node node;
			if (!ParseNode(node)) {
				return false;
			}
			root.children.push_back(std::move(node));
		}
		if (!m_ok) {
			return false;
		}
		// A KV1 file usually is `"Root" { ... }`; Valve's loader merges that single top-level
		// block into the object it parses into (pisexcompat did the same).
		if (root.children.size() == 1 && !root.children.front().children.empty()) {
			Kv1Node top = std::move(root.children.front());
			root.children.clear();
			root.name = std::move(top.name);
			root.value = std::move(top.value);
			root.hasValue = top.hasValue;
			root.children = std::move(top.children);
		}
		return true;
	}

private:
	bool AtEnd() const { return m_position >= m_text.size(); }

	char Peek() const { return AtEnd() ? '\0' : m_text[m_position]; }

	void SkipSpaceAndComments() {
		while (!AtEnd()) {
			const char character = m_text[m_position];
			if (std::isspace(static_cast<unsigned char>(character)) != 0) {
				++m_position;
				continue;
			}
			// `//` to the end of the line; KeyValues files in this project use only this form.
			if (character == '/' && m_position + 1 < m_text.size() && m_text[m_position + 1] == '/') {
				m_position += 2;
				while (!AtEnd() && m_text[m_position] != '\n') {
					++m_position;
				}
				continue;
			}
			break;
		}
	}

	bool ParseQuoted(std::string& out) {
		++m_position; // opening quote
		while (!AtEnd()) {
			const char character = m_text[m_position++];
			if (character == '"') {
				return true;
			}
			if (character == '\\' && !AtEnd()) {
				const char escaped = m_text[m_position++];
				switch (escaped) {
				case 'n': out.push_back('\n'); break;
				case 't': out.push_back('\t'); break;
				case '\\': out.push_back('\\'); break;
				case '"': out.push_back('"'); break;
				default: out.push_back(escaped); break;
				}
				continue;
			}
			out.push_back(character);
		}
		m_ok = false;
		return false;
	}

	bool ParseToken(std::string& out) {
		while (!AtEnd()) {
			const char character = m_text[m_position];
			if (std::isspace(static_cast<unsigned char>(character)) != 0 || character == '{' || character == '}'
				|| character == '"') {
				break;
			}
			out.push_back(character);
			++m_position;
		}
		return !out.empty();
	}

	bool ParseNode(Kv1Node& node) {
		SkipSpaceAndComments();
		if (AtEnd()) {
			return true;
		}
		if (Peek() == '}') {
			m_ok = false; // unbalanced closing brace
			return false;
		}
		if (Peek() == '"') {
			if (!ParseQuoted(node.name)) {
				return false;
			}
		} else if (!ParseToken(node.name)) {
			m_ok = false;
			return false;
		}

		SkipSpaceAndComments();
		if (AtEnd()) {
			return true; // a bare key: no value, no block
		}
		if (Peek() == '{') {
			++m_position;
			while (true) {
				SkipSpaceAndComments();
				if (AtEnd()) {
					m_ok = false; // missing closing brace
					return false;
				}
				if (Peek() == '}') {
					++m_position;
					return true;
				}
				Kv1Node child;
				if (!ParseNode(child)) {
					return false;
				}
				node.children.push_back(std::move(child));
			}
		}

		if (Peek() == '"') {
			if (!ParseQuoted(node.value)) {
				return false;
			}
		} else if (!ParseToken(node.value)) {
			m_ok = false;
			return false;
		}
		node.hasValue = true;
		return true;
	}

	std::string_view m_text;
	std::size_t m_position = 0;
	bool m_ok = true;
};

} // namespace

const Kv1Node* Kv1Node::Find(std::string_view key) const {
	for (const Kv1Node& child : children) {
		if (EqualsNoCase(child.name, key)) {
			return &child;
		}
	}
	return nullptr;
}

std::string Kv1Node::GetString(std::string_view key, std::string_view fallback) const {
	const Kv1Node* child = Find(key);
	if (child == nullptr || !child->hasValue) {
		return std::string(fallback);
	}
	return child->value;
}

int Kv1Node::GetInt(std::string_view key, int fallback) const {
	const Kv1Node* child = Find(key);
	if (child == nullptr || !child->hasValue || child->value.empty()) {
		return fallback;
	}
	errno = 0;
	char* end = nullptr;
	const long parsed = std::strtol(child->value.c_str(), &end, 10);
	if (end == child->value.c_str() || errno == ERANGE) {
		return fallback;
	}
	return static_cast<int>(parsed);
}

bool ParseKv1(std::string_view text, Kv1Node& root) {
	root = Kv1Node {};
	Parser parser(text);
	if (!parser.Parse(root)) {
		root = Kv1Node {};
		return false;
	}
	return true;
}

} // namespace k4
