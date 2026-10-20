// kproc on Linux (include/utils/process.hpp): the child-process code that
// dig_plugin.cpp, ai_manager.cpp, playback.cpp and voice2text.cpp had, moved
// here unchanged - posix_spawn with dup2 file actions + closefrom(3)
// (spawn_fds.hpp), pipe2(O_CLOEXEC), O_NONBLOCK parent ends, poll(),
// waitpid(), kill() / killpg(); fork() + exec where posix_spawn has no
// attribute for it (a nice level: the test-signal generator).
#ifndef _WIN32

#include "utils/process.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "utils/spawn_fds.hpp"

extern char** environ;

namespace kproc {

namespace {

void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

void close_fd(int& fd) {
    if (fd >= 0) ::close(fd);
    fd = -1;
}

std::vector<char*> c_argv(const std::vector<std::string>& a) {
    std::vector<char*> v;
    for (const auto& s : a) v.push_back(const_cast<char*>(s.c_str()));
    v.push_back(nullptr);
    return v;
}

int nice_of(Priority p) { return p == Priority::Idle ? 19 : p == Priority::Below ? 10 : 0; }

// environ with env_unset removed and env_set added / replaced
std::vector<std::string> make_env(const SpawnOptions& o) {
    std::vector<std::string> env;
    for (char** e = environ; e && *e; e++) {
        const char* eq = std::strchr(*e, '=');
        const std::string name(*e, eq ? static_cast<size_t>(eq - *e) : std::strlen(*e));
        bool drop = std::find(o.env_unset.begin(), o.env_unset.end(), name) != o.env_unset.end();
        for (const auto& kv : o.env_set) drop = drop || kv.first == name;
        if (!drop) env.emplace_back(*e);
    }
    for (const auto& kv : o.env_set) env.push_back(kv.first + "=" + kv.second);
    return env;
}

}  // namespace

// ---- executables / PATH ----------------------------------------------------
std::string exe_path(const std::string& path_without_suffix) { return path_without_suffix; }

bool is_executable(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(path.c_str(), X_OK) == 0;
}

std::string find_in_path(const std::string& name) {
    if (name.empty()) return "";
    if (name.find('/') != std::string::npos) return is_executable(name) ? name : "";
    const char* p = std::getenv("PATH");
    std::string path = p ? p : "/bin:/usr/bin";   // glibc execvp's default
    size_t b = 0;
    for (;;) {
        const size_t e = path.find(PATH_LIST_SEP, b);
        std::string dir = path.substr(b, e == std::string::npos ? std::string::npos : e - b);
        if (dir.empty()) dir = ".";   // an empty entry is the current directory (execvp)
        const std::string cand = dir + "/" + name;
        if (is_executable(cand)) return cand;
        if (e == std::string::npos) break;
        b = e + 1;
    }
    return "";
}

const std::vector<std::string>& python_argv() {
    static const std::vector<std::string> v{"python3"};
    return v;
}

std::vector<std::string> python_cmd(std::vector<std::string> script_and_args) {
    const auto& py = python_argv();
    if (py.empty()) return {};
    std::vector<std::string> v = py;
    v.insert(v.end(), std::make_move_iterator(script_and_args.begin()), std::make_move_iterator(script_and_args.end()));
    return v;
}

void lower_own_priority() { setpriority(PRIO_PROCESS, 0, 10); }

// ---- ExitStatus --------------------------------------------------------------
ExitStatus ExitStatus::from_wait_status(int st) {
    ExitStatus s;
    s.raw = static_cast<uint32_t>(st);
    if (WIFSIGNALED(st)) {
        s.kind = Kind::Signaled;
        s.signal = WTERMSIG(st);
    } else {
        s.kind = Kind::Exited;
        s.code = WEXITSTATUS(st);
    }
    return s;
}

std::string ExitStatus::signal_name() const {
    const char* n = strsignal(signal);
    return n ? n : "signal " + std::to_string(signal);
}

std::string ExitStatus::describe() const {
    return signaled() ? "crashed (" + signal_name() + ")" : "exited with status " + std::to_string(code);
}

// ---- PipePair ----------------------------------------------------------------
struct PipePair::Impl {
    int rfd = -1, wfd = -1;
};

PipePair::PipePair() : d_(std::make_unique<Impl>()) {}
PipePair::~PipePair() { close(); }

bool PipePair::create(size_t buffer_bytes, std::string* err) {
    close();
    int p[2];
    if (pipe2(p, O_CLOEXEC) != 0) {
        if (err) *err = std::string("pipe failed: ") + strerror(errno);
        return false;
    }
    d_->rfd = p[0];
    d_->wfd = p[1];
#ifdef F_SETPIPE_SZ
    if (buffer_bytes) fcntl(p[1], F_SETPIPE_SZ, static_cast<int>(buffer_bytes));   // best effort
#else
    (void)buffer_bytes;
#endif
    return true;
}

void PipePair::close() {
    close_fd(d_->rfd);
    close_fd(d_->wfd);
}

// ---- Process -----------------------------------------------------------------
struct Process::Impl {
    // pid / reaped / sig_sent under mu: kill() and terminate() may run on
    // another thread than the one waiting, and a pid must not be signalled
    // once it has been reaped (it can belong to a new process then)
    std::mutex mu;
    pid_t pid = -1;
    bool reaped = false;
    bool kill_tree = false;
    int sig_sent = 0;
    ExitStatus status;
    int in = -1, out = -1, err = -1;   // parent ends, O_NONBLOCK

    int& fd_of(Stream s) { return s == Stream::Err ? err : out; }
    void reap_locked(int st) {
        reaped = true;
        status = ExitStatus::from_wait_status(st);
        status.killed_by_us = sig_sent != 0 && status.signaled() && status.signal == sig_sent;
    }
    void signal_child(int sig) {
        std::lock_guard<std::mutex> lk(mu);
        if (pid <= 0 || reaped) return;
        sig_sent = sig;
        if (kill_tree) killpg(pid, sig);
        else ::kill(pid, sig);
    }
};

Process::Process() : d_(std::make_unique<Impl>()) {}
Process::~Process() { close(); }

bool Process::started() const { return d_->pid > 0; }

long Process::pid() const {
    std::lock_guard<std::mutex> lk(d_->mu);
    return d_->pid;
}

bool Process::spawn(const SpawnOptions& o, std::string* err, int* err_code) {
    auto fail = [&](int code, const std::string& text) {
        if (err) *err = text;
        if (err_code) *err_code = code;
        return false;
    };
    close();
    if (o.argv.empty()) return fail(EINVAL, "no command");
    signal(SIGPIPE, SIG_IGN);

    // pipes: [0] = read end, [1] = write end; the child's end is dup2'ed onto
    // its fd 0 / 1 / 2, the parent keeps the other one
    int pin[2] = {-1, -1}, pout[2] = {-1, -1}, perr[2] = {-1, -1};
    auto close_all = [&] {
        for (int* p : {pin, pout, perr}) {
            close_fd(p[0]);
            close_fd(p[1]);
        }
    };
    if ((o.in == Stdio::Pipe && pipe2(pin, O_CLOEXEC) != 0) || (o.out == Stdio::Pipe && pipe2(pout, O_CLOEXEC) != 0) ||
        (o.err == Stdio::Pipe && pipe2(perr, O_CLOEXEC) != 0)) {
        const int e = errno;
        close_all();
        return fail(e, "pipe failed");
    }
#ifdef F_SETPIPE_SZ
    // ~3 s of 38.4 kHz samples; array plugins' bursts in fewer, larger
    // writes - best effort, the system limit (pipe-max-size, 1 MB by
    // default) rules
    if (o.in == Stdio::Pipe && o.in_pipe_bytes) fcntl(pin[1], F_SETPIPE_SZ, static_cast<int>(o.in_pipe_bytes));
#endif
    const int pair_r = (o.in == Stdio::Handle && o.in_pair) ? o.in_pair->d_->rfd : -1;
    const int pair_w = (o.out == Stdio::Handle && o.out_pair) ? o.out_pair->d_->wfd : -1;
    if ((o.in == Stdio::Handle && pair_r < 0) || (o.out == Stdio::Handle && pair_w < 0)) {
        close_all();
        return fail(EBADF, "no pipe pair");
    }

    auto av = c_argv(o.argv);
    std::vector<std::string> env_s;
    std::vector<char*> env_p;
    char** envp = environ;
    if (!o.env_set.empty() || !o.env_unset.empty()) {
        env_s = make_env(o);
        env_p = c_argv(env_s);
        envp = env_p.data();
    }
    pid_t pid = -1;
    int rc = 0;
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 29))
    const bool spawn_chdir = true;   // posix_spawn_file_actions_addchdir_np
#else
    const bool spawn_chdir = false;
#endif
    if (o.priority == Priority::Normal && (o.cwd.empty() || spawn_chdir)) {
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        if (o.in == Stdio::Pipe) posix_spawn_file_actions_adddup2(&fa, pin[0], 0);
        else if (o.in == Stdio::Null) posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
        else if (o.in == Stdio::Handle) posix_spawn_file_actions_adddup2(&fa, pair_r, 0);
        if (o.out == Stdio::Pipe) posix_spawn_file_actions_adddup2(&fa, pout[1], 1);
        else if (o.out == Stdio::Null) posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
        else if (o.out == Stdio::Handle) posix_spawn_file_actions_adddup2(&fa, pair_w, 1);
        if (o.err == Stdio::Pipe) posix_spawn_file_actions_adddup2(&fa, perr[1], 2);
        else if (o.err == Stdio::Null) posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
        else if (o.err == Stdio::SameAsStdout) posix_spawn_file_actions_adddup2(&fa, o.out == Stdio::Pipe ? pout[1] : 1, 2);
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 29))
        if (!o.cwd.empty()) posix_spawn_file_actions_addchdir_np(&fa, o.cwd.c_str());
#endif
        spawn_fds::close_above_stdio(&fa);
        posix_spawnattr_t at;
        posix_spawnattr_init(&at);
        if (o.kill_tree) {
            posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);   // own group: kill() reaches what it starts
            posix_spawnattr_setpgroup(&at, 0);
        }
        rc = o.search_path ? posix_spawnp(&pid, av[0], &fa, &at, av.data(), envp)
                           : posix_spawn(&pid, av[0], &fa, &at, av.data(), envp);
        posix_spawn_file_actions_destroy(&fa);
        posix_spawnattr_destroy(&at);
    } else {
        // posix_spawn has no nice level: fork + exec, exec failure = exit 127
        const int nice_level = nice_of(o.priority);
        pid = fork();
        if (pid < 0) {
            rc = errno;
        } else if (pid == 0) {
            if (o.kill_tree) setpgid(0, 0);
            if (nice_level) setpriority(PRIO_PROCESS, 0, nice_level);   // the receiver comes first
            if (o.in == Stdio::Pipe) dup2(pin[0], 0);
            else if (o.in == Stdio::Handle) dup2(pair_r, 0);
            else if (o.in == Stdio::Null) {
                const int dn = ::open("/dev/null", O_RDONLY);
                if (dn >= 0) dup2(dn, 0);
            }
            if (o.out == Stdio::Pipe) dup2(pout[1], 1);
            else if (o.out == Stdio::Handle) dup2(pair_w, 1);
            else if (o.out == Stdio::Null) {
                const int dn = ::open("/dev/null", O_WRONLY);
                if (dn >= 0) dup2(dn, 1);
            }
            if (o.err == Stdio::Pipe) dup2(perr[1], 2);
            else if (o.err == Stdio::SameAsStdout) dup2(1, 2);
            else if (o.err == Stdio::Null) {
                const int dn = ::open("/dev/null", O_WRONLY);
                if (dn >= 0) dup2(dn, 2);
            }
            if (!o.cwd.empty() && chdir(o.cwd.c_str()) != 0) _exit(127);
            spawn_fds::close_above_stdio_in_child();
            environ = envp;
            if (o.search_path) execvp(av[0], av.data());
            else execv(av[0], av.data());
            _exit(127);
        }
    }
    // the child's ends belong to the child now
    close_fd(pin[0]);
    close_fd(pout[1]);
    close_fd(perr[1]);
    if (rc != 0 || pid <= 0) {
        close_all();
        return fail(rc ? rc : ECHILD, strerror(rc ? rc : ECHILD));
    }
    {
        std::lock_guard<std::mutex> lk(d_->mu);
        d_->pid = pid;
        d_->reaped = false;
        d_->kill_tree = o.kill_tree;
        d_->sig_sent = 0;
        d_->status = ExitStatus{};
    }
    d_->in = pin[1];
    d_->out = pout[0];
    d_->err = perr[0];
    for (int fd : {d_->in, d_->out, d_->err})
        if (fd >= 0) set_nonblock(fd);
    return true;
}

ptrdiff_t Process::write_some(const void* p, size_t n) {
    if (d_->in < 0) return -1;
    for (;;) {
        const ssize_t w = ::write(d_->in, p, n);
        if (w >= 0) return w;
        if (errno == EINTR) continue;
        return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;   // EPIPE: it died
    }
}

ptrdiff_t Process::read_some(Stream s, void* p, size_t n) {
    const int fd = d_->fd_of(s);
    if (fd < 0) return -1;
    for (;;) {
        const ssize_t r = ::read(fd, p, n);
        if (r > 0) return r;
        if (r == 0) return -1;
        if (errno == EINTR) continue;
        return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -2;
    }
}

ptrdiff_t Process::read(Stream s, void* p, size_t n, int timeout_ms) {
    const int fd = d_->fd_of(s);
    if (fd < 0) return -1;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(timeout_ms, 0));
    for (;;) {
        const ptrdiff_t r = read_some(s, p, n);
        if (r != 0) return r;
        int left = -1;
        if (timeout_ms >= 0) {
            left = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
            if (left <= 0) return 0;
        }
        pollfd pf{fd, POLLIN, 0};
        const int pr = ::poll(&pf, 1, left);
        if (pr < 0 && errno != EINTR) return -2;
        if (pr == 0) return 0;
    }
}

void Process::wait_io(int ms, bool want_read_out, bool want_write_in) {
    pollfd pf[2];
    int n = 0;
    if (want_read_out && d_->out >= 0) pf[n++] = {d_->out, POLLIN, 0};
    if (want_write_in && d_->in >= 0) pf[n++] = {d_->in, POLLOUT, 0};
    if (n) ::poll(pf, static_cast<nfds_t>(n), ms);
}

bool Process::stdin_open() const { return d_->in >= 0; }

void Process::close_stdin() { close_fd(d_->in); }   // EOF: the child exits on its own

bool Process::try_wait(ExitStatus* st) {
    std::lock_guard<std::mutex> lk(d_->mu);
    if (d_->pid <= 0) return false;
    if (!d_->reaped) {
        int ws = 0;
        if (waitpid(d_->pid, &ws, WNOHANG) != d_->pid) return false;
        d_->reap_locked(ws);
    }
    if (st) *st = d_->status;
    return true;
}

ExitStatus Process::wait() {
    pid_t pid;
    {
        std::lock_guard<std::mutex> lk(d_->mu);
        if (d_->pid <= 0) return ExitStatus{};
        if (d_->reaped) return d_->status;
        pid = d_->pid;
    }
    // block until it has exited WITHOUT reaping it, then reap under mu - a
    // kill() on another thread meanwhile still signals our own child
    siginfo_t si{};
    while (waitid(P_PID, static_cast<id_t>(pid), &si, WEXITED | WNOWAIT) < 0 && errno == EINTR) {}
    std::lock_guard<std::mutex> lk(d_->mu);
    if (!d_->reaped) {
        int ws = 0;
        while (waitpid(pid, &ws, 0) < 0 && errno == EINTR) {}
        d_->reap_locked(ws);
    }
    return d_->status;
}

bool Process::wait_for(int ms, ExitStatus* st) {
    if (ms <= 0) return try_wait(st);
    for (int i = 0; i < (ms + 9) / 10; i++) {
        if (try_wait(st)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

void Process::kill() { d_->signal_child(SIGKILL); }

void Process::terminate() { d_->signal_child(SIGTERM); }

void Process::close() {
    if (started()) {
        ExitStatus st;
        if (!try_wait(&st)) {   // not reaped: no zombie left behind (as the destructor)
            kill();
            wait();
        }
    }
    close_fd(d_->in);
    close_fd(d_->out);
    close_fd(d_->err);
    std::lock_guard<std::mutex> lk(d_->mu);
    d_->pid = -1;
    d_->reaped = false;
    d_->sig_sent = 0;
}

// ---- run_capture -------------------------------------------------------------
bool run_capture(const std::vector<std::string>& argv, int timeout_ms, std::string* out, ExitStatus* st,
                 size_t max_bytes, bool search_path, Console console) {
    (void)console;
    Process p;
    SpawnOptions o;
    o.argv = argv;
    o.search_path = search_path;
    o.in = Stdio::Null;
    o.out = Stdio::Pipe;
    o.err = Stdio::Null;
    if (!p.spawn(o, nullptr)) return false;
    out->clear();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    bool timed_out = false;
    for (;;) {
        const int left = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
        if (left <= 0) {
            timed_out = true;
            break;
        }
        char buf[4096];
        const ptrdiff_t n = p.read(Stream::Out, buf, sizeof buf, left);
        if (n == 0) {
            timed_out = true;
            break;
        }
        if (n == -2) {   // poll / read failed: our end stays open until the reap - don't let it block on it
            p.kill();
            break;
        }
        if (n < 0) break;   // EOF
        if (out->size() < max_bytes) out->append(buf, static_cast<size_t>(n));
    }
    if (timed_out) p.kill();
    const ExitStatus s = p.wait();
    p.close();
    if (st) *st = s;
    return !timed_out && s.ok();
}

}  // namespace kproc

#endif  // !_WIN32
