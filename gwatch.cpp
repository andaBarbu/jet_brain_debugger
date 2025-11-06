#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <sys/uio.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>

#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstring>

#include <iostream>
#include <sstream>
#include <vector>
#include <string>
#include <thread>
#include <chrono>

using u64 = unsigned long long;
using u32 = unsigned long;


static void fatal_perror_and_exit(const char *msg, pid_t child = -1) {
    perror(msg);
    if (child > 0) {
        ptrace(PTRACE_DETACH, child, nullptr, nullptr);
    }
    exit(2);
}

static long must_ptrace(enum __ptrace_request request, pid_t pid, void *addr, long data, const char *what) {
    errno = 0;
    long r = ptrace(request, pid, addr, (void*)data);
    if (r == -1 && errno != 0) {
        fprintf(stderr, "ptrace(%s) failed: ", what);
        fatal_perror_and_exit(what, pid);
    }
    return r;
}

bool read_child_mem(pid_t pid, uintptr_t addr, void* buf, size_t size) {
    struct iovec local[1];
    struct iovec remote[1];
    local[0].iov_base = buf;
    local[0].iov_len = size;
    remote[0].iov_base = (void*)addr;
    remote[0].iov_len = size;
    ssize_t n = process_vm_readv(pid, local, 1, remote, 1, 0);
    return n == (ssize_t)size;
}

uintptr_t resolveSymbolOffset(const std::string &exePath, const std::string &symbol) {
    std::string cmd = "nm -C " + exePath + " | grep ' " + symbol + "$' | head -n1";
    FILE *pipe = popen(cmd.c_str(), "r");
    if (!pipe) return 0;

    char buf[512];
    std::string line;
    if (fgets(buf, sizeof(buf), pipe)) line = buf;

    pclose(pipe);
    if (line.empty()) return 0;

    std::istringstream iss(line);
    std::string addrStr;
    iss >> addrStr;
    if (addrStr == "w" || addrStr == "U" || addrStr == "T") return 0;

    try { return std::stoull(addrStr, nullptr, 16); } catch (...) { return 0; }
}

uintptr_t findModuleBase(pid_t pid, const std::string &exePath) {
    char mapsPath[64];
    snprintf(mapsPath, sizeof(mapsPath), "/proc/%d/maps", pid);
    FILE *f = fopen(mapsPath, "r");
    if (!f) return 0;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        std::string s = line;
        size_t pos = s.find('/');
        if (pos == std::string::npos) continue;
        std::string path = s.substr(pos);

        while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) path.pop_back();

        if (path.size() >= exePath.size() && path.find(exePath) != std::string::npos) {
            std::stringstream ss(s);
            std::string addrRange;
            ss >> addrRange;

            size_t dash = addrRange.find('-');
            if (dash != std::string::npos) {
                uintptr_t base = std::stoull(addrRange.substr(0, dash), nullptr, 16);
                fclose(f);
                return base;
            }
        }
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 5) {
        std::cerr << "Usage: gwatch --var <symbol> --exec <path> [-- arg1 ...]\n";
        return 1;
    }

    std::string varName;
    std::string exePath;
    std::vector<std::string> childArgs;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (s == "--var" && i + 1 < argc) varName = argv[++i];
        else if (s == "--exec" && i + 1 < argc) exePath = argv[++i];
        else if (s == "--") {
            for (int j = i + 1; j < argc; ++j) childArgs.push_back(argv[j]);
            break;
        }
    }

    if (varName.empty() || exePath.empty()) {
        std::cerr << "Missing --var or --exec\n";
        return 1;
    }

    uintptr_t symOffset = resolveSymbolOffset(exePath, varName);
    if (symOffset == 0) {
        std::cerr << "Symbol not found: " << varName << "\n";
        return 1;
    }
    std::cerr << "Symbol offset: 0x" << std::hex << symOffset << std::dec << "\n";

    std::vector<char*> argv_exec;
    argv_exec.push_back(const_cast<char*>(exePath.c_str()));
    for (auto &a : childArgs) argv_exec.push_back(const_cast<char*>(a.c_str()));
    argv_exec.push_back(nullptr);

    pid_t pid = fork();
    if (pid == -1) { perror("fork"); return 1; }

    if (pid == 0) {
        if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr) == -1) perror("ptrace TRACEME");
        execv(exePath.c_str(), argv_exec.data());
        perror("execv");
        _exit(1);
    }

    int status;
    if (waitpid(pid, &status, 0) == -1) { perror("waitpid"); return 1; }
    if (WIFEXITED(status)) { std::cerr << "Child exited prematurely\n"; return 1; }

    uintptr_t base = findModuleBase(pid, exePath);
    if (base == 0) {
        char exeLink[128];
        snprintf(exeLink, sizeof(exeLink), "/proc/%d/exe", pid);
        char realpathBuf[1024];
        ssize_t len = readlink(exeLink, realpathBuf, sizeof(realpathBuf)-1);
        if (len > 0) {
            realpathBuf[len] = '\0';
            std::string realExe = realpathBuf;
            base = findModuleBase(pid, realExe);
        }
    }

    uintptr_t runtimeAddr = (base == 0) ? symOffset : (base + symOffset);
    std::cerr << "Resolved runtime address: 0x" << std::hex << runtimeAddr << std::dec << "\n";

    must_ptrace(PTRACE_POKEUSER, pid, (void*)offsetof(struct user, u_debugreg[0]), (long)runtimeAddr, "poke DR0");
    unsigned long dr7 = 0;
    dr7 |= 1;
    dr7 |= (3 << 16);
    dr7 |= (3 << 18);
    must_ptrace(PTRACE_POKEUSER, pid, (void*)offsetof(struct user, u_debugreg[7]), (long)dr7, "poke DR7");

    errno = 0;
    long prevVal = ptrace(PTRACE_PEEKDATA, pid, (void*)runtimeAddr, nullptr);
    if (prevVal == -1 && errno != 0) perror("ptrace peekdata initial");

    must_ptrace(PTRACE_CONT, pid, nullptr, 0, "PTRACE_CONT initial");

    while (true) {
        if (waitpid(pid, &status, 0) == -1) { perror("waitpid"); break; }
        if (WIFEXITED(status)) break;
        if (WIFSTOPPED(status)) {
            int sig = WSTOPSIG(status);
            if (sig == SIGTRAP) {
                long curVal = 0;
                if (!read_child_mem(pid, runtimeAddr, &curVal, sizeof(curVal))) {
                    errno = 0;
                    long v = ptrace(PTRACE_PEEKDATA, pid, (void*)runtimeAddr, nullptr);
                    if (v == -1 && errno != 0) perror("peekdata");
                    else curVal = v;
                }

                if (curVal != prevVal) {
                    std::cout << varName << " write " << prevVal << " -> " << curVal << "\n";
                    prevVal = curVal;
                } else {
                    std::cout << varName << " read " << curVal << "\n";
                }

                must_ptrace(PTRACE_CONT, pid, nullptr, 0, "PTRACE_CONT after trap");
            } else {
                must_ptrace(PTRACE_CONT, pid, nullptr, (long)sig, "PTRACE_CONT forward signal");
            }
        }
    }
    must_ptrace(PTRACE_POKEUSER, pid, (void*)offsetof(struct user, u_debugreg[7]), 0, "clear DR7");

    return 0;
}
