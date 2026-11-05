// Small non-validating XML reader for SOAP responses. Namespaces are matched
// by local name, which is all the ONVIF calls used here need. DTDs are refused.
#include "internal.hpp"

#include <cstdlib>

namespace ptz {

namespace {

std::string local_name(std::string_view name) {
    auto colon = name.find(':');
    return std::string(colon == std::string_view::npos ? name : name.substr(colon + 1));
}

void append_utf8(std::string &out, unsigned long code) {
    if (code < 0x80) out.push_back(static_cast<char>(code));
    else if (code < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x110000) {
        out.push_back(static_cast<char>(0xF0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else throw Error("Invalid XML character reference");
}

std::string decode(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '&') { out.push_back(raw[i]); continue; }
        auto end = raw.find(';', i);
        if (end == std::string_view::npos) throw Error("Malformed XML entity");
        auto entity = raw.substr(i + 1, end - i - 1);
        if (entity == "lt") out.push_back('<');
        else if (entity == "gt") out.push_back('>');
        else if (entity == "amp") out.push_back('&');
        else if (entity == "quot") out.push_back('"');
        else if (entity == "apos") out.push_back('\'');
        else if (!entity.empty() && entity[0] == '#') {
            bool hex = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X');
            std::string digits(entity.substr(hex ? 2 : 1));
            char *stop = nullptr;
            unsigned long code = std::strtoul(digits.c_str(), &stop, hex ? 16 : 10);
            if (digits.empty() || *stop) throw Error("Malformed XML character reference");
            append_utf8(out, code);
        } else throw Error("Unknown XML entity");
        i = end;
    }
    return out;
}

class Parser {
public:
    explicit Parser(const std::string &text) : s_(text) {}

    XmlNode document() {
        skip_misc();
        if (peek() != '<') throw Error("Camera response is not XML");
        XmlNode root = element(0);
        skip_misc();
        return root;
    }

private:
    char peek() const { return i_ < s_.size() ? s_[i_] : '\0'; }
    bool starts(std::string_view token) const { return s_.compare(i_, token.size(), token) == 0; }

    void skip_space() {
        while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_;
    }

    void skip_until(std::string_view token) {
        auto end = s_.find(token, i_);
        if (end == std::string::npos) throw Error("Truncated XML in camera response");
        i_ = end + token.size();
    }

    void skip_misc() {
        while (true) {
            skip_space();
            if (starts("<?")) skip_until("?>");
            else if (starts("<!--")) skip_until("-->");
            else if (starts("<!")) throw Error("XML document type declarations are not accepted");
            else return;
        }
    }

    std::string name() {
        size_t start = i_;
        while (i_ < s_.size()) {
            char c = s_[i_];
            if (std::isspace(static_cast<unsigned char>(c)) || c == '>' || c == '/' || c == '=') break;
            ++i_;
        }
        if (start == i_) throw Error("Malformed XML name");
        return s_.substr(start, i_ - start);
    }

    XmlNode element(int depth) {
        if (depth > 64) throw Error("XML nesting too deep");
        ++i_;  // '<'
        XmlNode node;
        std::string raw_name = name();
        node.name = local_name(raw_name);
        while (true) {
            skip_space();
            if (starts("/>")) { i_ += 2; return node; }
            if (peek() == '>') { ++i_; break; }
            std::string attribute = name();
            skip_space();
            if (peek() != '=') throw Error("Malformed XML attribute");
            ++i_;
            skip_space();
            char quote = peek();
            if (quote != '"' && quote != '\'') throw Error("Malformed XML attribute value");
            auto end = s_.find(quote, ++i_);
            if (end == std::string::npos) throw Error("Truncated XML attribute");
            if (attribute.rfind("xmlns", 0) != 0)
                node.attributes[local_name(attribute)] = decode(std::string_view(s_).substr(i_, end - i_));
            i_ = end + 1;
        }
        while (true) {
            if (i_ >= s_.size()) throw Error("Truncated XML in camera response");
            if (starts("</")) {
                i_ += 2;
                if (name() != raw_name) throw Error("Mismatched XML closing tag");
                skip_space();
                if (peek() != '>') throw Error("Malformed XML closing tag");
                ++i_;
                node.text = trim(node.text);
                return node;
            }
            if (starts("<!--")) skip_until("-->");
            else if (starts("<![CDATA[")) {
                i_ += 9;
                auto end = s_.find("]]>", i_);
                if (end == std::string::npos) throw Error("Truncated XML CDATA");
                node.text += s_.substr(i_, end - i_);
                i_ = end + 3;
            } else if (starts("<?")) skip_until("?>");
            else if (peek() == '<') node.children.push_back(element(depth + 1));
            else {
                auto end = s_.find('<', i_);
                if (end == std::string::npos) end = s_.size();
                node.text += decode(std::string_view(s_).substr(i_, end - i_));
                i_ = end;
            }
        }
    }

    const std::string &s_;
    size_t i_ = 0;
};

}  // namespace

const XmlNode *XmlNode::child(std::string_view local) const {
    for (const auto &c : children)
        if (c.name == local) return &c;
    return nullptr;
}

std::vector<const XmlNode *> XmlNode::children_named(std::string_view local) const {
    std::vector<const XmlNode *> result;
    for (const auto &c : children)
        if (c.name == local) result.push_back(&c);
    return result;
}

const XmlNode *XmlNode::find(std::string_view local) const {
    if (name == local) return this;
    for (const auto &c : children)
        if (auto found = c.find(local)) return found;
    return nullptr;
}

std::string XmlNode::child_text(std::string_view local) const {
    auto c = child(local);
    return c ? c->text : std::string();
}

std::string XmlNode::attribute(std::string_view local) const {
    auto it = attributes.find(std::string(local));
    return it == attributes.end() ? std::string() : it->second;
}

XmlNode xml_parse(const std::string &document) { return Parser(document).document(); }

std::string xml_escape(std::string_view text) {
    std::string out;
    for (char c : text) {
        switch (c) {
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '&': out += "&amp;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out.push_back(c);
        }
    }
    return out;
}

}  // namespace ptz
