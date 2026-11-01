#include "core/cpu_topology.h"

#include <dirent.h>
#include <sched.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

namespace judas {

std::vector<int> parse_cpu_list(const std::string & s) {
    std::vector<int> out;
    std::stringstream ss(s);
    std::string part;
    while (std::getline(ss, part, ',')) {
        size_t a = part.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) continue;
        part = part.substr(a);
        try {
            size_t dash = part.find('-');
            if (dash == std::string::npos) {
                out.push_back(std::stoi(part));
            } else {
                int lo = std::stoi(part.substr(0, dash)), hi = std::stoi(part.substr(dash + 1));
                for (int i = lo; i <= hi; i++) out.push_back(i);
            }
        } catch (...) {}
    }
    return out;
}

static std::string slurp(const std::string & p) {
    std::ifstream f(p);
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return s;
}

std::vector<int> cpu_pin_order(const std::string & root) {
    std::vector<int> cpus;
    if (DIR * d = opendir(root.c_str())) {
        while (dirent * e = readdir(d)) {
            std::string n = e->d_name;
            if (n.size() > 3 && n.compare(0, 3, "cpu") == 0 && n.find_first_not_of("0123456789", 3) == std::string::npos)
                cpus.push_back(std::stoi(n.substr(3)));
        }
        closedir(d);
    }
    std::sort(cpus.begin(), cpus.end());
    // group key per CPU: L3 domain (CCD) = lowest cpu sharing index3, else the package id
    std::map<int, std::vector<int>> by_ccd;  // ccd id (lowest cpu in the domain) -> physical cores' primary threads
    std::vector<int> siblings;
    std::set<int> seen;
    for (int c : cpus) {
        std::string base = root + "/cpu" + std::to_string(c);
        std::vector<int> sib = parse_cpu_list(slurp(base + "/topology/thread_siblings_list"));
        if (sib.empty()) sib = {c};
        int primary = *std::min_element(sib.begin(), sib.end());
        std::vector<int> l3 = parse_cpu_list(slurp(base + "/cache/index3/shared_cpu_list"));
        int ccd = l3.empty() ? 0 : *std::min_element(l3.begin(), l3.end());
        if (c == primary) {
            if (seen.insert(c).second) by_ccd[ccd].push_back(c);
        } else {
            siblings.push_back(c);
        }
    }
    // siblings grouped the same way: order them by their primary's position
    std::vector<int> order;
    for (auto & [ccd, v] : by_ccd) {
        std::sort(v.begin(), v.end());
        order.insert(order.end(), v.begin(), v.end());
    }
    std::vector<int> sib_order;
    for (int p : order) {
        for (int s : siblings) {
            std::vector<int> sib = parse_cpu_list(slurp(root + "/cpu" + std::to_string(s) + "/topology/thread_siblings_list"));
            if (!sib.empty() && *std::min_element(sib.begin(), sib.end()) == p) sib_order.push_back(s);
        }
    }
    order.insert(order.end(), sib_order.begin(), sib_order.end());
    return order;
}

int cpu_count() {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int) n : 1;
}

std::vector<int> cpu_pin_set(int n, const std::string & root) {
    std::vector<int> order = cpu_pin_order(root);
    if (order.empty())
        for (int i = 0; i < cpu_count(); i++) order.push_back(i);
    n = std::clamp(n, 1, (int) order.size());
    order.resize(n);
    return order;
}

static cpu_set_t g_original;
static bool g_have_original = false;
static std::mutex g_mu;

std::vector<int> process_affinity() {
    cpu_set_t s;
    CPU_ZERO(&s);
    std::vector<int> out;
    if (sched_getaffinity(0, sizeof(s), &s) != 0) return out;
    for (int i = 0; i < CPU_SETSIZE; i++)
        if (CPU_ISSET(i, &s)) out.push_back(i);
    return out;
}

bool pin_process(const std::vector<int> & cpus) {
    std::lock_guard lk(g_mu);
    if (!g_have_original) {
        sched_getaffinity(0, sizeof(g_original), &g_original);
        g_have_original = true;
    }
    cpu_set_t want;
    if (cpus.empty()) {
        want = g_original;
    } else {
        CPU_ZERO(&want);
        for (int c : cpus)
            if (c >= 0 && c < CPU_SETSIZE) CPU_SET(c, &want);
    }
    bool ok = true;
    if (DIR * d = opendir("/proc/self/task")) {
        while (dirent * e = readdir(d)) {
            if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
            if (sched_setaffinity((pid_t) std::stoi(e->d_name), sizeof(want), &want) != 0) ok = false;
        }
        closedir(d);
    } else {
        ok = sched_setaffinity(0, sizeof(want), &want) == 0;
    }
    return ok;
}

}  // namespace judas
