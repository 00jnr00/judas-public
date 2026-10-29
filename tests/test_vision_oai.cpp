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

int main() {
    test_data_url();
    test_remote_url_ignored();
    test_plain_text_unchanged();
    TEST_MAIN_END();
}
