// Offline checks for image input on the OpenAI-compatible path: data URLs become ChatMessage::images,
// text parts still concatenate, other URLs are ignored. No model needed.
#include "engine/engine.h"
#include "test.h"

using namespace judas;

static std::string b64(const std::string & in) {
    static const char * tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < in.size(); i += 3) {
        unsigned v = (unsigned char) in[i] << 16;
        if (i + 1 < in.size()) v |= (unsigned char) in[i + 1] << 8;
        if (i + 2 < in.size()) v |= (unsigned char) in[i + 2];
        out += tbl[v >> 18];
        out += tbl[(v >> 12) & 63];
        out += i + 1 < in.size() ? tbl[(v >> 6) & 63] : '=';
        out += i + 2 < in.size() ? tbl[v & 63] : '=';
    }
    return out;
}

static void test_data_url() {
    std::string png = std::string("\x89PNG\r\n\x1a\n", 8) + std::string("\0\1\2\xff\xfe", 5) + "tail";
    for (size_t cut = 0; cut < 3; cut++) {  // every padding case
        std::string bytes = png.substr(0, png.size() - cut);
        json msgs = json::array({{{"role", "user"},
                                  {"content", json::array({{{"type", "text"}, {"text", "what is "}},
                                                           {{"type", "image_url"},
                                                            {"image_url", {{"url", "data:image/png;base64," + b64(bytes)}}}},
                                                           {{"type", "text"}, {"text", "this?"}}})}}});
        auto out = messages_from_oai(msgs);
        REQUIRE(out.size() == 1);
        CHECK(out[0].content == "what is this?");
        REQUIRE(out[0].images.size() == 1);
        CHECK(out[0].images[0] == bytes);
    }
}

static void test_remote_url_ignored() {
    json msgs = json::parse(R"([{"role":"user","content":[
        {"type":"text","text":"hi"},
        {"type":"image_url","image_url":{"url":"http://example.com/a.png"}},
        {"type":"image_url","image_url":"https://example.com/b.png"}]}])");
    auto out = messages_from_oai(msgs);
    REQUIRE(out.size() == 1);
    CHECK(out[0].content == "hi");
    CHECK(out[0].images.empty());
}

static void test_plain_text_unchanged() {
    json msgs = json::array({{{"role", "user"}, {"content", "plain"}}});
    auto out = messages_from_oai(msgs);
    REQUIRE(out.size() == 1);
    CHECK(out[0].content == "plain");
    CHECK(out[0].images.empty());
    CHECK(message_to_oai(out[0])["content"] == "plain");
}

// Over-escaped LaTeX in tool arguments: the doubled form copied into a raw parameter is repaired; already-correct
// strings, non-math strings and non-JSON stay as they are.
static void test_tool_args_latex() {
    json bad = {{"correctAnswer", "$-\\\\dfrac{1}{\\\\tau}e^{-t/\\\\tau}$"},
                {"options", json::array({json{{"label", "$-\\dfrac{1}{\\tau}e^{-t/\\tau}$"}}})},
                {"path", "C:\\\\Users\\\\x"}};
    json fixed = json::parse(fix_tool_args_latex(bad.dump()));
    CHECK(fixed["correctAnswer"] == "$-\\dfrac{1}{\\tau}e^{-t/\\tau}$");
    CHECK(fixed["correctAnswer"] == fixed["options"][0]["label"]);   // now matches the option exactly
    CHECK(fixed["path"] == "C:\\\\Users\\\\x");                    // no math, untouched
    json good = {{"q", "$\\tau$ is a time constant"}};
    CHECK(fix_tool_args_latex(good.dump()) == good.dump());
    CHECK(fix_tool_args_latex("not json \\\\tau") == "not json \\\\tau");
    CHECK(fix_tool_args_latex("{}") == "{}");
    json quoted = {{"a", "\"$-\\\\dfrac{1}{\\\\tau}$\""}, {"b", "\"$x$\""}, {"c", "\"plain quoted\""}};
    json uq = json::parse(fix_tool_args_latex(quoted.dump()));
    CHECK(uq["a"] == "$-\\dfrac{1}{\\tau}$" && uq["b"] == "$x$" && uq["c"] == "\"plain quoted\"");
}

int main() {
    test_tool_args_latex();
    test_data_url();
    test_remote_url_ignored();
    test_plain_text_unchanged();
    TEST_MAIN_END();
}
