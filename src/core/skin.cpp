#include "core/skin.h"

#include <filesystem>
#include <sstream>

#include <toml++/toml.hpp>

#include "core/config.h"  // expand_home

namespace judas {

namespace fs = std::filesystem;

std::string Skin::color(const std::string & key) const {
    auto it = colors.find(key);
    return it == colors.end() ? "" : it->second;
}

bool Skin::parse_hex(const std::string & hex, int & r, int & g, int & b) {
    std::string h = hex;
    if (!h.empty() && h[0] == '#') h = h.substr(1);
    if (h.size() != 6) return false;
    for (char c : h)
        if (!std::isxdigit((unsigned char) c)) return false;
    r = std::stoi(h.substr(0, 2), nullptr, 16);
    g = std::stoi(h.substr(2, 2), nullptr, 16);
    b = std::stoi(h.substr(4, 2), nullptr, 16);
    return true;
}

std::vector<Skin> load_skins(const std::vector<std::string> & dirs) {
    std::vector<Skin> out;
    out.push_back(Skin{});  // built-in judas first
    for (const auto & d : dirs) {
        std::error_code ec;
        fs::path dir = expand_home(d);
        if (!fs::is_directory(dir, ec)) continue;
        for (const auto & e : fs::directory_iterator(dir, ec)) {
            if (e.path().extension() != ".toml") continue;
            try {
                auto t = toml::parse_file(e.path().string());
                Skin s;
                s.name = t["name"].value_or(e.path().stem().string());
                s.label = t["label"].value_or(s.name);
                s.dark = t["dark"].value_or(true);
                if (auto * c = t["colors"].as_table())
                    for (auto && [k, v] : *c)
                        if (auto str = v.value<std::string>()) s.colors[std::string(k.str())] = *str;
                // a skin with the same name replaces an earlier one
                std::erase_if(out, [&](const Skin & x) { return x.name == s.name; });
                out.push_back(std::move(s));
            } catch (...) {
                // skip a malformed skin file
            }
        }
    }
    return out;
}

Skin skin_by_name(const std::string & name, const std::vector<std::string> & dirs) {
    for (const auto & s : load_skins(dirs))
        if (s.name == name) return s;
    return Skin{};
}

std::string skin_to_css(const Skin & s) {
    std::ostringstream o;
    o << "/* judas skin: " << s.name << " */\n:root {\n";
    o << "  color-scheme: " << (s.dark ? "dark" : "light") << ";\n";
    for (const auto & [k, v] : s.colors) o << "  --" << k << ": " << v << ";\n";
    // a translucent accent for focus rings and hovers
    int r, g, b;
    if (Skin::parse_hex(s.color("accent"), r, g, b))
        o << "  --accent-soft: rgba(" << r << "," << g << "," << b << ",0.16);\n";
    o << "}\n";
    return o.str();
}

}  // namespace judas
