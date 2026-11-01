// CPU topology and process pinning for the CPU-only swarm instance (docs/SWARM.md, "Cores").
#pragma once
#include <string>
#include <vector>

namespace judas {

// Logical CPUs in the order a pinned process should take them: first one thread per physical core, filling a
// CCD (L3 domain) before the next, then the SMT siblings in the same order. On a 5900X that is 0-5, 6-11, 12-17,
// 18-23. `sysfs_root` is /sys/devices/system/cpu (a fake tree in tests). Empty when the tree cannot be read.
std::vector<int> cpu_pin_order(const std::string & sysfs_root = "/sys/devices/system/cpu");

// Online logical CPUs (nproc).
int cpu_count();

// The first n entries of cpu_pin_order (n clamped to 1..size), or 0..n-1 when the topology is unreadable.
std::vector<int> cpu_pin_set(int n, const std::string & sysfs_root = "/sys/devices/system/cpu");

// sched_setaffinity on every thread of this process (threads created later inherit from their creator).
// An empty list restores the mask the process started with. Returns false if the kernel refused.
bool pin_process(const std::vector<int> & cpus);

// The CPUs the process may currently run on.
std::vector<int> process_affinity();

// "0-5,12" style list parsing, as found in sysfs (used by tests too).
std::vector<int> parse_cpu_list(const std::string & s);

}  // namespace judas
