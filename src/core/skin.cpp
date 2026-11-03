#include "core/skin.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <toml++/toml.hpp>

#include "core/config.h"  // expand_home
#include "core/skin_assets.h"

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

bool parse_skin_toml(const std::string & text, const std::string & fallback_name, Skin & out) {
    try {
        auto t = toml::parse(text);
        Skin s;
        s.name = t["name"].value_or(fallback_name);
        s.label = t["label"].value_or(s.name);
        s.dark = t["dark"].value_or(true);
        if (auto * c = t["colors"].as_table())
            for (auto && [k, v] : *c)
                if (auto str = v.value<std::string>()) s.colors[std::string(k.str())] = *str;
        if (auto * c = t["tokens"].as_table())
            for (auto && [k, v] : *c)
                if (auto str = v.value<std::string>()) s.tokens[std::string(k.str())] = *str;
        out = std::move(s);
        return true;
    } catch (...) {
        return false;  // malformed skin file
    }
}

std::vector<Skin> load_skins(const std::vector<std::string> & dirs) {
    std::vector<Skin> out;
    out.push_back(Skin{});  // built-in judas first
    auto add = [&](Skin s) {
        // a skin with the same name replaces an earlier one
        std::erase_if(out, [&](const Skin & x) { return x.name == s.name; });
        out.push_back(std::move(s));
    };
    size_t n = 0;
    const SkinAsset * assets = skin_assets(n);
    for (size_t i = 0; i < n; i++) {
        Skin s;
        if (parse_skin_toml(assets[i].text, fs::path(assets[i].file).stem().string(), s)) add(std::move(s));
    }
    for (const auto & d : dirs) {
        std::error_code ec;
        fs::path dir = expand_home(d);
        if (!fs::is_directory(dir, ec)) continue;
        std::vector<fs::path> files;
        for (const auto & e : fs::directory_iterator(dir, ec))
            if (e.path().extension() == ".toml") files.push_back(e.path());
        std::sort(files.begin(), files.end());
        for (const auto & f : files) {
            std::ifstream in(f);
            std::stringstream ss;
            ss << in.rdbuf();
            Skin s;
            if (parse_skin_toml(ss.str(), f.stem().string(), s)) add(std::move(s));
        }
    }
    return out;
}

Skin skin_by_name(const std::string & name, const std::vector<std::string> & dirs) {
    for (const auto & s : load_skins(dirs))
        if (s.name == name) return s;
    return Skin{};
}

std::string Skin::token(const std::string & key) const {
    auto it = tokens.find(key);
    return it == tokens.end() ? "" : it->second;
}

std::map<std::string, std::string> skin_vars(const Skin & s) {
    std::map<std::string, std::string> v;
    for (const auto & [k, c] : s.colors) v["--" + k] = c;
    for (const auto & [k, t] : s.tokens) {
        std::string name = k;
        std::replace(name.begin(), name.end(), '_', '-');
        v["--" + name] = t;
    }
    // a translucent accent for focus rings and hovers
    int r, g, b;
    if (Skin::parse_hex(s.color("accent"), r, g, b))
        v["--accent-soft"] = "rgba(" + std::to_string(r) + "," + std::to_string(g) + "," + std::to_string(b) + ",0.16)";
    return v;
}

std::string skin_to_css(const Skin & s) {
    std::ostringstream o;
    o << "/* judas skin: " << s.name << " */\n:root {\n";
    o << "  color-scheme: " << (s.dark ? "dark" : "light") << ";\n";
    for (const auto & [k, v] : skin_vars(s)) o << "  " << k << ": " << v << ";\n";
    o << "}\n";
    return o.str();
}

nlohmann::json skin_to_json(const Skin & s) {
    nlohmann::json vars = nlohmann::json::object();
    for (const auto & [k, v] : skin_vars(s)) vars[k] = v;
    return {{"name", s.name}, {"label", s.label}, {"dark", s.dark}, {"vars", vars}};
}

}  // namespace judas
