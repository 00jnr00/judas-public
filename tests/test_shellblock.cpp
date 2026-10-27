// Offline check: the shell tool refuses sudo/credential commands in full-auto, still runs safe ones.
#include "core/config.h"
#include "snap/journal.h"
#include "tools/tools.h"
#include "test.h"
using namespace judas;
int main() {
    Config cfg;
    cfg.permissions.mode = "full-auto";
    cfg.permissions.allow_paths = {"/tmp"};
    ToolRegistry reg; register_builtin_tools(reg);
    Journal j(JUDAS_TEST_TMP "/judas-block-j");
    auto call = [&](const std::string & c) {
        ToolContext ctx; ctx.cwd="/tmp"; ctx.cfg=&cfg; ctx.journal=&j;
        ctx.checkpoint=j.begin("s","a",c); auto r=reg.call("shell", {{"command",c}}, ctx); j.commit(ctx.checkpoint);
        return r;
    };
    const char * blocked[] = {"sudo -S whoami", "echo x | sudo -S id", "for p in a b; do echo $p|sudo -S id; done",
                              "su root", "passwd", "faillock --reset", "doas id", "echo bad > /etc/shadow",
                              "/usr/bin/sudo -S id", "\"sudo\" -S id", "\\sudo -S id", "'/usr/bin/su' -c id",
                              "cp /tmp/evil /etc/passwd", "sed -i s/x/y/ /etc/passwd", "cat /etc/shadow",
                              "for i in 1; do sudo id; done", "env FOO=1 sudo id", "LANG=C sudo id", "xargs -n1 sudo < /dev/null"};
    for (auto c : blocked) { auto r=call(c); printf("blocked? %-45s ok=%d %.60s\n", c, r.ok, r.output.c_str()); CHECK(!r.ok && r.output.rfind("refused", 0) == 0); }
    const char * allowed[] = {"echo hello", "ls /tmp >/dev/null", "python3 -c 'print(1+1)'", "pseudonym=1; echo $pseudonym",
                              "head -1 /etc/passwd >/dev/null", "getent passwd root >/dev/null", "man -w sudo >/dev/null 2>&1 || true"};
    for (auto c : allowed) { auto r=call(c); printf("allowed? %-45s ok=%d\n", c, r.ok); CHECK(r.ok); }
    TEST_MAIN_END();
}
