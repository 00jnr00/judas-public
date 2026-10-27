// See schema.h.
#include "mcp/schema.h"

#include <cmath>
#include <vector>

namespace judas::mcp {

namespace {

const char * kind_name(const json & v) {
    if (v.is_null()) return "null";
    if (v.is_boolean()) return "boolean";
    if (v.is_number_integer()) return "integer";
    if (v.is_number()) return "number";
    if (v.is_string()) return "string";
    if (v.is_array()) return "array";
    return "object";
}

bool type_ok(const std::string & t, const json & v) {
    if (t == "string") return v.is_string();
    if (t == "boolean") return v.is_boolean();
    if (t == "null") return v.is_null();
    if (t == "array") return v.is_array();
    if (t == "object") return v.is_object();
    if (t == "integer") return v.is_number_integer() || (v.is_number_float() && v.get<double>() == std::floor(v.get<double>()));
    if (t == "number") return v.is_number();
    return true;  // unknown type name: do not reject
}

bool is_required(const json & s, const std::string & key) {
    if (!s.contains("required") || !s["required"].is_array()) return false;
    for (auto & r : s["required"]) if (r.is_string() && r.get_ref<const std::string &>() == key) return true;
    return false;
}

struct Checker {
    std::vector<std::string> errs;
    static constexpr size_t kMax = 5;

    void err(const std::string & path, const std::string & msg) {
        if (errs.size() < kMax) errs.push_back((path.empty() ? "arguments" : path) + ": " + msg);
    }

    // true when v matches schema with no errors (used for anyOf/oneOf without recording).
    bool matches(const json & schema, const json & v) {
        Checker sub;
        sub.check(schema, v, "");
        return sub.errs.empty();
    }

    void check(const json & s, const json & v, const std::string & path) {
        if (!s.is_object()) return;  // true / unknown: anything goes
        if (errs.size() >= kMax) return;

        if (s.contains("const") && s["const"] != v) {
            err(path, "must equal " + s["const"].dump());
            return;
        }
        if (s.contains("enum") && s["enum"].is_array()) {
            bool found = false;
            for (auto & e : s["enum"]) if (e == v) { found = true; break; }
            if (!found) {
                std::string opts;
                for (auto & e : s["enum"]) opts += (opts.empty() ? "" : ", ") + e.dump();
                err(path, "must be one of " + opts + ", got " + v.dump().substr(0, 80));
                return;
            }
        }
        if (s.contains("type")) {
            std::vector<std::string> types;
            if (s["type"].is_string()) types.push_back(s["type"].get<std::string>());
            else if (s["type"].is_array())
                for (auto & t : s["type"]) if (t.is_string()) types.push_back(t.get<std::string>());
            if (!types.empty()) {
                bool ok = false;
                std::string want;
                for (auto & t : types) {
                    want += (want.empty() ? "" : " or ") + t;
                    if (type_ok(t, v)) ok = true;
                }
                if (!ok) {
                    err(path, "expected " + want + ", got " + kind_name(v));
                    return;
                }
            }
        }
        if (s.contains("allOf") && s["allOf"].is_array())
            for (auto & sub : s["allOf"]) check(sub, v, path);
        for (const char * k : {"anyOf", "oneOf"}) {
            if (!s.contains(k) || !s[k].is_array() || s[k].empty()) continue;
            bool any = false;
            for (auto & sub : s[k]) if (matches(sub, v)) { any = true; break; }
            if (!any) {
                err(path, std::string("does not match any allowed form (") + k + ")");
                return;
            }
        }

        if (v.is_number()) {
            double d = v.get<double>();
            if (s.contains("minimum") && s["minimum"].is_number() && d < s["minimum"].get<double>())
                err(path, "must be >= " + s["minimum"].dump());
            if (s.contains("maximum") && s["maximum"].is_number() && d > s["maximum"].get<double>())
                err(path, "must be <= " + s["maximum"].dump());
            if (s.contains("exclusiveMinimum") && s["exclusiveMinimum"].is_number() && d <= s["exclusiveMinimum"].get<double>())
                err(path, "must be > " + s["exclusiveMinimum"].dump());
            if (s.contains("exclusiveMaximum") && s["exclusiveMaximum"].is_number() && d >= s["exclusiveMaximum"].get<double>())
                err(path, "must be < " + s["exclusiveMaximum"].dump());
        }
        if (v.is_string()) {
            const std::string & str = v.get_ref<const std::string &>();
            // length in code points
            size_t cp = 0;
            for (unsigned char c : str) if ((c & 0xC0) != 0x80) cp++;
            if (s.contains("minLength") && s["minLength"].is_number_integer() && (long long) cp < s["minLength"].get<long long>())
                err(path, "must be at least " + s["minLength"].dump() + " characters");
            if (s.contains("maxLength") && s["maxLength"].is_number_integer() && (long long) cp > s["maxLength"].get<long long>())
                err(path, "must be at most " + s["maxLength"].dump() + " characters");
        }
        if (v.is_array()) {
            if (s.contains("minItems") && s["minItems"].is_number_integer() && (long long) v.size() < s["minItems"].get<long long>())
                err(path, "needs at least " + s["minItems"].dump() + " items");
            if (s.contains("maxItems") && s["maxItems"].is_number_integer() && (long long) v.size() > s["maxItems"].get<long long>())
                err(path, "allows at most " + s["maxItems"].dump() + " items");
            if (s.contains("items") && s["items"].is_object())
                for (size_t i = 0; i < v.size(); i++) check(s["items"], v[i], path + "[" + std::to_string(i) + "]");
        }
        if (v.is_object()) {
            if (s.contains("required") && s["required"].is_array()) {
                std::string missing;
                for (auto & r : s["required"]) {
                    if (!r.is_string()) continue;
                    const std::string & k = r.get_ref<const std::string &>();
                    if (!v.contains(k) || v[k].is_null()) missing += (missing.empty() ? "" : ", ") + k;
                }
                if (!missing.empty()) err(path, "missing required argument(s): " + missing);
            }
            const json props = s.contains("properties") && s["properties"].is_object() ? s["properties"] : json::object();
            for (auto it = v.begin(); it != v.end(); ++it) {
                std::string sub = path.empty() ? it.key() : path + "." + it.key();
                if (props.contains(it.key())) {
                    // a null for an optional argument is treated as "not given"
                    if (it.value().is_null() && !is_required(s, it.key())) continue;
                    check(props[it.key()], it.value(), sub);
                } else if (s.contains("additionalProperties")) {
                    const json & ap = s["additionalProperties"];
                    if (ap.is_boolean() && !ap.get<bool>()) err(path, "unknown argument '" + it.key() + "'");
                    else if (ap.is_object()) check(ap, it.value(), sub);
                }
            }
        }
    }
};

}  // namespace

std::string validate_args(const json & schema, const json & args) {
    Checker c;
    c.check(schema, args, "");
    if (c.errs.empty()) return "";
    std::string out = "invalid arguments: ";
    for (size_t i = 0; i < c.errs.size(); i++) out += (i ? "; " : "") + c.errs[i];
    if (schema.is_object() && schema.contains("properties") && schema["properties"].is_object()) {
        std::string names;
        for (auto it = schema["properties"].begin(); it != schema["properties"].end(); ++it)
            names += (names.empty() ? "" : ", ") + it.key();
        out += ". Accepted arguments: " + names;
    }
    return out + ". Nothing ran; fix the arguments and call again.";
}

}  // namespace judas::mcp
