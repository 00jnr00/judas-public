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
                              "for i in 1; do sudo id; done", "env FOO=1 sudo id", "LANG=C sudo id", "xargs -n1 sudo < /dev/null",
                              // wrappers, quoting tricks, IFS, escapes, globs, more names, more write verbs
                              "bash -c 'sudo id'", "sh -c \"echo hi; sudo id\"", "eval \"sudo id\"", "busybox su", "sudo${IFS}id",
                              "env${IFS}sudo id", "s\"\"udo id", "su\\do id", "$'\\x73udo' id", "/usr/bin/sud? id", "/usr/bin/s[u]do id",
                              "{sudo,id}", "sudo<x", "sudo-rs id", "sudoedit /etc/hosts", "timeout 5 sudo id", "useradd evil",
                              "machinectl shell root@", "gpasswd -a me wheel", "vipw", "cat /etc/shad*", "cat /etc/sudoers.d/x",
                              "sed -n -i s/a/b/ /etc/passwd", "sed --in-place s/a/b/ /etc/group", "echo x >| /etc/passwd",
                              "perl -pi -e s/a/b/ /etc/passwd"};
    for (auto c : blocked) { auto r=call(c); printf("blocked? %-45s ok=%d %.60s\n", c, r.ok, r.output.c_str()); CHECK(!r.ok && r.output.rfind("refused", 0) == 0); }
    const char * allowed[] = {"echo hello", "ls /tmp >/dev/null", "python3 -c 'print(1+1)'", "pseudonym=1; echo $pseudonym",
                              "head -1 /etc/passwd >/dev/null", "getent passwd root >/dev/null", "man -w sudo >/dev/null 2>&1 || true",
                              "bash -c 'echo hi'", "ls /tmp/* >/dev/null 2>&1 || true", "grep sudo /dev/null || true", "echo \"use sudo carefully\"",
                              "sed -n 1p /etc/passwd >/dev/null", "sed -i s/a/b/ /tmp/judas-nonexistent 2>/dev/null || true"};
    for (auto c : allowed) { auto r=call(c); printf("allowed? %-45s ok=%d\n", c, r.ok); CHECK(r.ok); }
    TEST_MAIN_END();
}
