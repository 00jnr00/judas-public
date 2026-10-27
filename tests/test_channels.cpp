// Channels tests: command delivery, selection, disabled, unknown kind, unreachable ntfy (no real network).
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "channels/channels.h"
#include "test.h"

using namespace judas;

static Channel mk(const std::string & name, const std::string & kind, const std::string & target, bool en = true) {
    Channel c;
    c.name = name; c.kind = kind; c.target = target; c.enabled = en;
    return c;
}

static std::string slurp(const std::string & p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int main() {
    std::filesystem::create_directories(JUDAS_TEST_TMP);
    std::string out = JUDAS_TEST_TMP "/test_channels_" + std::to_string(getpid()) + ".txt";
    std::filesystem::remove(out);

    ChannelsConfig cfg;
    cfg.channels.push_back(mk("file", "command", "cat > " + out + "; printf '|%s' \"$JUDAS_NOTIFY_TITLE\" >> " + out));
    cfg.channels.push_back(mk("fail", "command", "echo boom; exit 1"));
    cfg.channels.push_back(mk("off", "command", "true", false));
    cfg.channels.push_back(mk("weird", "pigeon", "x"));
    cfg.channels.push_back(mk("ntfy", "ntfy", "http://127.0.0.1:1/x"));
    cfg.def = "file";
    Channels ch(cfg);

    auto names = ch.names();
    CHECK(names.size() == 5 && names[0] == "file");

    auto r = ch.send("file", "Hello Title", "body text");
    CHECK(r.ok);
    CHECK(slurp(out) == "body text|Hello Title");

    // default selection
    std::filesystem::remove(out);
    r = ch.send("", "T2", "b2");
    CHECK(r.ok);
    CHECK(slurp(out) == "b2|T2");
    std::filesystem::remove(out);

    r = ch.send("missing", "t", "b");
    CHECK(!r.ok && r.error == "no such channel");

    r = ch.send("fail", "t", "b");
    CHECK(!r.ok && r.error == "boom");

    r = ch.send("off", "t", "b");
    CHECK(!r.ok && r.error == "channel disabled");

    r = ch.send("weird", "t", "b");
    CHECK(!r.ok && r.error == "unknown channel kind");

    r = ch.send("ntfy", "t", "b");
    CHECK(!r.ok && !r.error.empty());

    // single channel and no default: used when name is empty
    ChannelsConfig one;
    one.channels.push_back(mk("only", "command", "cat >/dev/null"));
    CHECK(Channels(one).send("", "t", "b").ok);

    // no channels
    CHECK(!Channels(ChannelsConfig{}).send("", "t", "b").ok);

    // large body must not deadlock
    CHECK(Channels(one).send("only", "t", std::string(300000, 'x')).ok);

    std::filesystem::remove(out);
    TEST_MAIN_END();
}
