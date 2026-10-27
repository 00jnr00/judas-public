#include "mcp/schema.h"
#include "test.h"

using namespace judas;
using judas::mcp::validate_args;

int main() {
    json schema = {{"type", "object"},
                   {"properties",
                    {{"path", {{"type", "string"}, {"minLength", 1}}},
                     {"n", {{"type", "integer"}, {"minimum", 1}, {"maximum", 10}}},
                     {"mode", {{"type", "string"}, {"enum", {"a", "b"}}}},
                     {"tags", {{"type", "array"}, {"items", {{"type", "string"}}}, {"maxItems", 2}}},
                     {"opt", {{"type", {"string", "null"}}}},
                     {"nested", {{"type", "object"}, {"properties", {{"x", {{"type", "number"}}}}}, {"required", {"x"}}}}}},
                   {"required", {"path"}}};

    CHECK(validate_args(schema, {{"path", "a.txt"}}).empty());
    CHECK(validate_args(schema, {{"path", "a"}, {"n", 3}, {"mode", "b"}, {"tags", {"x", "y"}}, {"nested", {{"x", 1.5}}}}).empty());
    CHECK(validate_args(schema, {{"path", "a"}, {"n", 3.0}}).empty());  // 3.0 is an integer value

    auto e = validate_args(schema, json::object());
    CHECK(e.find("missing required argument(s): path") != std::string::npos);
    CHECK(e.find("Accepted arguments: path, n, mode") != std::string::npos);

    e = validate_args(schema, {{"path", 5}});
    CHECK(e.find("path: expected string, got integer") != std::string::npos);
    e = validate_args(schema, {{"path", "a"}, {"n", "3"}});
    CHECK(e.find("n: expected integer, got string") != std::string::npos);
    e = validate_args(schema, {{"path", "a"}, {"n", 0}});
    CHECK(e.find("n: must be >= 1") != std::string::npos);
    e = validate_args(schema, {{"path", "a"}, {"n", 11}});
    CHECK(e.find("n: must be <= 10") != std::string::npos);
    e = validate_args(schema, {{"path", "a"}, {"mode", "z"}});
    CHECK(e.find("mode: must be one of \"a\", \"b\"") != std::string::npos);
    e = validate_args(schema, {{"path", "a"}, {"tags", {"x", 2}}});
    CHECK(e.find("tags[1]: expected string, got integer") != std::string::npos);
    e = validate_args(schema, {{"path", "a"}, {"tags", {"x", "y", "z"}}});
    CHECK(e.find("tags: allows at most 2 items") != std::string::npos);
    e = validate_args(schema, {{"path", "a"}, {"nested", json::object()}});
    CHECK(e.find("nested: missing required argument(s): x") != std::string::npos);
    e = validate_args(schema, {{"path", ""}});
    CHECK(e.find("at least 1 characters") != std::string::npos);

    // optional null counts as "not given"; union types; unknown keys are fine unless additionalProperties is false
    CHECK(validate_args(schema, {{"path", "a"}, {"mode", nullptr}}).empty());
    CHECK(validate_args(schema, {{"path", "a"}, {"opt", nullptr}}).empty());
    CHECK(validate_args(schema, {{"path", "a"}, {"extra", 1}}).empty());
    json strict = {{"type", "object"}, {"properties", {{"a", {{"type", "string"}}}}}, {"additionalProperties", false}};
    CHECK(validate_args(strict, {{"b", 1}}).find("unknown argument 'b'") != std::string::npos);

    // anyOf / oneOf / const; keywords we do not know are ignored
    json any = {{"anyOf", {{{"type", "string"}}, {{"type", "integer"}}}}};
    CHECK(validate_args({{"type", "object"}, {"properties", {{"v", any}}}}, {{"v", 3}}).empty());
    CHECK(!validate_args({{"type", "object"}, {"properties", {{"v", any}}}}, {{"v", true}}).empty());
    CHECK(validate_args({{"type", "object"}, {"properties", {{"v", {{"$ref", "#/nope"}, {"format", "uri"}}}}}}, {{"v", 1}}).empty());
    CHECK(!validate_args({{"type", "object"}, {"properties", {{"v", {{"const", 4}}}}}}, {{"v", 5}}).empty());

    // at most 5 problems reported
    json many = {{"type", "object"}, {"properties", json::object()}};
    json bad = json::object();
    for (int i = 0; i < 9; i++) {
        many["properties"]["k" + std::to_string(i)] = {{"type", "string"}};
        bad["k" + std::to_string(i)] = i;
    }
    e = validate_args(many, bad);
    CHECK(e.find("k4") != std::string::npos && e.find("k5: expected") == std::string::npos);

    // a non-object schema accepts anything
    CHECK(validate_args(json(true), {{"a", 1}}).empty());
    TEST_MAIN_END();
}
