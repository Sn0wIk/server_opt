#include "server_opt.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <regex>
#include <spawn.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace opt {
volatile std::sig_atomic_t interrupted = 0;
namespace {
using Clock = std::chrono::steady_clock;

std::string trim(std::string value) {
    const auto begin = value.find_first_not_of(" \r\n\t");
    if (begin == std::string::npos) return {};
    return value.substr(begin, value.find_last_not_of(" \r\n\t") - begin + 1);
}

[[maybe_unused]] Json maybe_read(const std::filesystem::path& path) {
    try { return read_file(path); }
    catch (const std::exception&) { return nullptr; }
}

std::string docker(std::vector<std::string> arguments, double timeout = 60,
                   bool honor_interrupt = true) {
    arguments.insert(arguments.begin(), "docker");
    auto result = run_command(arguments, timeout, honor_interrupt);
    if (result.exit_code != 0)
        throw std::runtime_error("docker " + arguments[1] + " failed (exit " +
                                 std::to_string(result.exit_code) + ")");
    return trim(result.output);
}

struct Sample {
    double memory;
    Json raw;
};

Sample docker_stats(const std::string& container) {
    const auto raw = docker({"stats", "--no-stream", "--format",
                            "{{.MemUsage}}\t{{.CPUPerc}}\t{{.PIDs}}", container}, 15);
    return {memory_bytes(raw.substr(0, raw.find('\t'))),
            Json::Object{{"at", timestamp()}, {"memory_cpu_pids", raw}}};
}

struct ContainerCleanup {
    std::string name;
    ~ContainerCleanup() {
        try { docker({"rm", "--force", name}, 15, false); }
        catch (...) { std::cerr << "If present, remove container manually: " << name << '\n'; }
    }
};

struct Trial {
    std::string profile;
    BenchResult bench;
    std::optional<double> memory;
    bool valid = false;
    Json report;
};

Trial trial(const std::string& profile, const std::string& image,
            const ExperimentOptions& options) {
    static unsigned serial = 0;
    const auto container = "server-opt-" + std::to_string(getpid()) + "-" +
                           std::to_string(Clock::now().time_since_epoch().count()) + "-" +
                           std::to_string(++serial);
    ContainerCleanup cleanup{container};
    docker({"run", "--detach", "--name", container, "--label", "app=server-opt-experiment",
            "--cpus", std::to_string(options.cpus), "--memory", options.memory,
            "--memory-swap", options.memory, "--pids-limit", "512", "--read-only",
            "--tmpfs", "/tmp:rw,nosuid,nodev,size=32m", "--cap-drop", "ALL",
            "--security-opt", "no-new-privileges", "--publish", "127.0.0.1::8080",
            image, "nginx", "-c", "/etc/nginx/" + profile + ".conf", "-g", "daemon off;"});
    const auto binding = docker({"port", container, "8080/tcp"});
    if (binding.rfind("127.0.0.1:", 0) != 0 || binding.find('\n') != std::string::npos)
        throw std::runtime_error("Expected one localhost Docker port binding");
    BenchOptions load;
    load.url = "http://" + binding + "/health";
    load.duration = 0.05;
    load.concurrency = 1;
    load.timeout = 1;
    load.expected_body = payload;
    const auto ready_deadline = Clock::now() + std::chrono::seconds(20);
    bool ready = false;
    while (Clock::now() < ready_deadline && !interrupted) {
        const auto check = benchmark(load);
        if (check.successful && !check.failed) { ready = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (!ready)
        throw std::runtime_error("Nginx did not become ready. " + docker({"logs", container}));
    load.duration = 1;
    load.timeout = 3;
    load.concurrency = options.concurrency;
    const auto warmup = benchmark(load);
    if (warmup.failed || !warmup.successful) throw std::runtime_error("Warm-up failed");
    const auto idle = docker_stats(container);
    std::vector<Sample> samples;
    Json errors = Json::Array{};
    bool sample_failed = false;
    std::atomic<bool> stop{false};
    std::thread sampler([&] {
        while (!stop.load() && !interrupted) {
            try { samples.push_back(docker_stats(container)); }
            catch (const std::exception& error) {
                sample_failed = true;
                errors.push(error.what());
            }
            for (int i = 0; i < 10 && !stop.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
    load.duration = options.duration;
    BenchResult measured;
    try { measured = benchmark(load); }
    catch (...) {
        stop.store(true);
        sampler.join();
        throw;
    }
    stop.store(true);
    sampler.join();
    if (interrupted) throw std::runtime_error("Interrupted");
    const auto state = docker({"inspect", "--format",
                               "{{.State.Running}} {{.State.OOMKilled}}", container});
    // Docker maps host PIDs to container PIDs and requires a PID column.
    const auto processes = docker({"top", container, "-eo", "pid,comm"});
    std::istringstream lines(processes);
    std::string line;
    unsigned nginx_count = 0;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        std::string pid, command;
        if (fields >> pid >> command && command == "nginx") ++nginx_count;
    }
    std::vector<double> memories;
    Json raw = Json::Array{};
    for (auto& sample : samples) {
        memories.push_back(sample.memory);
        raw.push(std::move(sample.raw));
    }
    const auto memory = memories.empty() ? std::optional<double>{}
                                         : std::optional<double>{median(memories)};
    const auto maximum = memories.empty() ? Json()
                                          : Json(*std::max_element(memories.begin(), memories.end()));
    const bool valid = measured.successful && !measured.failed && !sample_failed &&
                       !samples.empty() && state == "true false";
    Json report = Json::Object{
        {"profile", profile}, {"benchmark", measured.report},
        {"nginx_process_count_including_master", nginx_count},
        {"idle_memory_bytes", idle.memory},
        {"load_memory_median_bytes", memory ? Json(*memory) : Json()},
        {"load_memory_max_sampled_bytes", maximum},
        {"docker_idle_stats", idle.raw}, {"docker_load_stats", raw},
        {"sampling_errors", errors}, {"container_running_oom_killed", state},
        {"measurement_valid", valid},
    };
    return {profile, std::move(measured), memory, valid, std::move(report)};
}
} // namespace

CommandResult run_command(const std::vector<std::string>& argv, double timeout,
                          bool honor_interrupt) {
    if (argv.empty()) throw std::invalid_argument("Empty command");
    int descriptors[2];
    if (pipe(descriptors) != 0) throw std::runtime_error("pipe failed");
    for (int fd : descriptors) fcntl(fd, F_SETFD, FD_CLOEXEC);
    std::vector<char*> arguments;
    for (const auto& arg : argv) arguments.push_back(const_cast<char*>(arg.c_str()));
    arguments.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, descriptors[0]);
    posix_spawn_file_actions_addclose(&actions, descriptors[1]);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);
    pid_t child = -1;
    const int error = posix_spawnp(&child, arguments[0], &actions, &attributes,
                                   arguments.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    close(descriptors[1]);
    if (error != 0) {
        close(descriptors[0]);
        throw std::runtime_error("Cannot start " + argv[0] + ": " + std::strerror(error));
    }
    fcntl(descriptors[0], F_SETFL, O_NONBLOCK);
    const auto deadline = Clock::now() + std::chrono::duration<double>(timeout);
    std::string output;
    bool eof = false, exited = false;
    int status = 0;
    auto terminate = [&] {
        kill(-child, SIGKILL);
        if (!exited) {
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
        }
        close(descriptors[0]);
    };
    try {
        while (!exited || !eof) {
            if (Clock::now() >= deadline)
                throw std::runtime_error(argv[0] + " timed out");
            if (honor_interrupt && interrupted) throw std::runtime_error("Interrupted");
            char buffer[8192];
            while (true) {
                const auto bytes = read(descriptors[0], buffer, sizeof(buffer));
                if (bytes > 0) {
                    output.append(buffer, static_cast<size_t>(bytes));
                    if (output.size() > 8 * max_body)
                        throw std::runtime_error("Command output exceeds 8 MiB");
                } else {
                    if (bytes == 0) eof = true;
                    else if (errno != EAGAIN && errno != EINTR)
                        throw std::runtime_error("Reading command output failed");
                    break;
                }
            }
            if (!exited) {
                const auto waited = waitpid(child, &status, WNOHANG);
                if (waited == child) exited = true;
                else if (waited < 0 && errno != EINTR)
                    throw std::runtime_error("waitpid failed");
            }
            if (!exited || !eof) {
                pollfd descriptor{descriptors[0], POLLIN, 0};
                if (eof) std::this_thread::sleep_for(std::chrono::milliseconds(10));
                else poll(&descriptor, 1, 50);
            }
        }
    } catch (...) { terminate(); throw; }
    close(descriptors[0]);
    return {WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status), output};
}

std::string read_file(const std::filesystem::path& path, size_t limit) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot read " + path.string());
    std::string result;
    char chunk[8192];
    while (stream) {
        stream.read(chunk, sizeof(chunk));
        result.append(chunk, static_cast<size_t>(stream.gcount()));
        if (result.size() > limit) throw std::runtime_error("File too large: " + path.string());
    }
    if (stream.bad()) throw std::runtime_error("Reading failed: " + path.string());
    return result;
}

std::string timestamp() {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
    gmtime_r(&now, &utc);
    std::ostringstream value;
    value << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return value.str();
}

void write_report(const Json& report, const std::filesystem::path& output, bool print) {
    const auto text = report.dump() + "\n";
    if (!output.empty()) {
        auto temporary = output;
        temporary += ".tmp-" + std::to_string(getpid());
        try {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            if (!file) throw std::runtime_error("Cannot write " + temporary.string());
            file << text;
            file.close();
            if (!file) throw std::runtime_error("Writing report failed");
            std::filesystem::rename(temporary, output);
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            throw;
        }
    }
    if (print) std::cout << text;
}

Json parse_meminfo(const std::string& text) {
    std::istringstream stream(text);
    std::map<std::string, uint64_t> fields;
    std::string line;
    while (std::getline(stream, line)) {
        std::istringstream words(line);
        std::string key, unit;
        uint64_t amount = 0;
        if (!(words >> key >> amount)) continue;
        words >> unit;
        if (!key.empty() && key.back() == ':') key.pop_back();
        fields[key] = amount * (unit == "kB" ? 1024 : 1);
    }
    auto get = [&](const std::string& key) -> Json {
        return fields.count(key) ? Json(fields.at(key)) : Json();
    };
    return Json::Object{
        {"total_bytes", get("MemTotal")}, {"available_bytes", get("MemAvailable")},
        {"used_excluding_reclaimable_bytes",
         fields.count("MemTotal") && fields.count("MemAvailable") &&
         fields["MemTotal"] >= fields["MemAvailable"]
             ? Json(fields["MemTotal"] - fields["MemAvailable"]) : Json()},
        {"swap_total_bytes", get("SwapTotal")}, {"swap_free_bytes", get("SwapFree")},
    };
}

Json audit() {
    utsname system{};
    if (uname(&system) != 0) throw std::runtime_error("uname failed");
    Json notes = Json::Array{};
    double loads[3]{};
    Json load = Json::Array{};
    const auto available_loads = getloadavg(loads, 3);
    for (int i = 0; i < available_loads; ++i) load.push(loads[i]);
    Json result = Json::Object{
        {"schema_version", 2}, {"created_at", timestamp()}, {"platform", system.sysname},
        {"kernel", system.release}, {"architecture", system.machine},
        {"logical_cpus", sysconf(_SC_NPROCESSORS_ONLN)},
        {"load_average_1_5_15", load},
        {"scope", "read-only snapshot; no settings or services changed"},
    };
    struct statvfs disk{};
    if (statvfs(".", &disk) == 0) {
        result["working_directory_filesystem"] = Json::Object{
            {"total_bytes", static_cast<uint64_t>(disk.f_blocks) * disk.f_frsize},
            {"free_bytes", static_cast<uint64_t>(disk.f_bavail) * disk.f_frsize},
        };
    }
#ifdef __linux__
    result["os_release"] = maybe_read("/etc/os-release");
    result["memory"] = parse_meminfo(read_file("/proc/meminfo"));
    Json pressure = Json::Object{};
    for (const auto* name : {"cpu", "memory", "io"})
        pressure[name] = maybe_read(std::string("/proc/pressure/") + name);
    result["pressure"] = pressure;
    Json cgroup = Json::Object{};
    for (const auto* name : {"cpu.max", "memory.current", "memory.max", "memory.events"})
        cgroup[name] = maybe_read(std::string("/sys/fs/cgroup/") + name);
    result["visible_cgroup_v2_root"] = cgroup;
    struct Process { std::string name; int pid; uint64_t rss; };
    std::vector<Process> processes;
    for (const auto& entry : std::filesystem::directory_iterator("/proc")) {
        const auto pid = entry.path().filename().string();
        if (pid.empty() || pid.find_first_not_of("0123456789") != std::string::npos) continue;
        try {
            std::istringstream status(read_file(entry.path() / "status"));
            Process process{"", std::stoi(pid), 0};
            std::string line;
            while (std::getline(status, line)) {
                if (line.rfind("Name:", 0) == 0) process.name = trim(line.substr(5));
                if (line.rfind("VmRSS:", 0) == 0)
                    process.rss = std::stoull(trim(line.substr(6))) * 1024;
            }
            processes.push_back(std::move(process));
        } catch (const std::exception&) { /* Process exited or permission denied. */ }
    }
    std::sort(processes.begin(), processes.end(),
              [](const Process& a, const Process& b) { return a.rss > b.rss; });
    Json largest = Json::Array{};
    for (size_t i = 0; i < std::min<size_t>(15, processes.size()); ++i)
        largest.push(Json::Object{{"pid", processes[i].pid}, {"name", processes[i].name},
                                  {"rss_bytes", processes[i].rss}});
    result["top_processes_by_rss"] = largest;
    result["running_services"] = nullptr;
    try {
        auto services = run_command({"systemctl", "list-units", "--type=service",
                                    "--state=running", "--no-legend", "--plain", "--no-pager"}, 10);
        if (services.exit_code == 0) {
            Json units = Json::Array{};
            std::istringstream lines(services.output);
            std::string line, unit;
            while (std::getline(lines, line)) {
                std::istringstream words(line);
                if (words >> unit) units.push(unit);
            }
            result["running_services"] = units;
        }
    } catch (const std::exception&) {}
    notes.push("Memory/load can describe the host inside a container. The visible cgroup "
               "root need not be this process's cgroup.");
    notes.push("RSS includes shared pages; do not sum it as total RAM. "
               "Process arguments and environment variables are not collected.");
#else
    notes.push("Full audit requires Linux. This snapshot does not measure Ubuntu memory or services.");
#endif
    notes.push("Installed package count and CPU utilization do not measure energy per task.");
    result["notes"] = notes;
    return result;
}

double memory_bytes(const std::string& docker_memory) {
    static const std::regex pattern(R"(^\s*([0-9]+(?:\.[0-9]+)?)\s*([A-Za-z]+)\s*$)");
    static const std::map<std::string, double> units{
        {"B", 1}, {"kB", 1e3}, {"KB", 1e3}, {"MB", 1e6}, {"GB", 1e9},
        {"KiB", 1024}, {"MiB", 1048576}, {"GiB", 1073741824},
        {"TiB", 1099511627776.0},
    };
    const auto value = docker_memory.substr(0, docker_memory.find('/'));
    std::smatch match;
    if (!std::regex_match(value, match, pattern) || !units.count(match[2].str()))
        throw std::runtime_error("Unrecognized Docker memory value: " + docker_memory);
    const double bytes = std::stod(match[1].str()) * units.at(match[2].str());
    if (!std::isfinite(bytes)) throw std::runtime_error("Invalid Docker memory number");
    return bytes;
}

double median(std::vector<double> values) {
    if (values.empty()) throw std::invalid_argument("No values for median");
    std::sort(values.begin(), values.end());
    const auto mid = values.size() / 2;
    return values.size() % 2 ? values[mid] : (values[mid - 1] + values[mid]) / 2;
}

ExperimentResult experiment(const ExperimentOptions& options) {
    BenchOptions load;
    load.url = "http://127.0.0.1/";
    load.duration = options.duration;
    load.concurrency = options.concurrency;
    validate(load);
    if (options.duration < 2) throw std::invalid_argument("Experiment duration must be >= 2");
    if (!options.repeats || options.repeats > 20)
        throw std::invalid_argument("repeats must be between 1 and 20");
    if (!std::isfinite(options.cpus) || options.cpus <= 0)
        throw std::invalid_argument("cpus must be positive and finite");
    if (!std::regex_match(options.memory, std::regex("[1-9][0-9]*[kKmMgG]?")))
        throw std::invalid_argument("memory must be a positive size such as 256m");
    for (const auto* path : {"Dockerfile", "nginx/baseline.conf"})
        if (!std::filesystem::is_regular_file(options.project_root / path))
            throw std::runtime_error("Project root is missing " + std::string(path) +
                                     "; use --project-root");
    const auto info = docker({"info", "--format",
                             "{{.OSType}}\t{{.OperatingSystem}}\t{{.Architecture}}\t"
                             "{{.NCPU}}\t{{.MemTotal}}\t{{.CgroupVersion}}"});
    if (info.rfind("linux\t", 0) != 0)
        throw std::runtime_error("A local Linux Docker engine is required");
    const auto version = docker({"version", "--format", "{{.Server.Version}}"});
    std::cerr << "Building Ubuntu + Nginx image (first run needs internet)…\n";
    docker({"build", "--tag", "server-opt-nginx:local", options.project_root.string()}, 900);
    const auto image = docker({"image", "inspect", "--format", "{{.Id}}", "server-opt-nginx:local"});
    const auto packages = docker({"run", "--rm", "--network", "none", image,
                                  "dpkg-query", "-W", "-f=${Package}=${Version}\n"});
    utsname host{};
    uname(&host);
    Json report = Json::Object{
        {"schema_version", 2}, {"created_at", timestamp()},
        {"experiment", "Nginx worker_processes auto versus 1 under identical CPU/RAM limits"},
        {"host_platform", host.sysname}, {"host_architecture", host.machine},
        {"docker_server_version", version},
        {"docker_os_description_arch_cpus_memory_cgroup", info},
        {"image_id", image}, {"packages", packages},
        {"baseline_config", read_file(options.project_root / "nginx/baseline.conf")},
        {"settings", Json::Object{
            {"cpus", options.cpus}, {"memory", options.memory},
            {"repeats", options.repeats}, {"duration", options.duration},
            {"concurrency", options.concurrency},
        }},
        {"trials", Json::Array{}},
        {"limitations", Json::Array{
            "Synthetic tiny HTTP response, no TLS, application or database.",
            "Only worker_processes differs; auto may see more CPUs than the CPU quota.",
            "If auto creates one worker there is no tuning difference.",
            "Docker Desktop measures Linux VM containers, not physical Ubuntu or energy.",
            "Docker memory is cache-adjusted and rounded; sampled max is not the actual peak.",
            "Load generator and Docker share the host; runs are exploratory.",
            "Ubuntu and apt packages change; preserve the recorded image for reproducibility.",
            "Containers are removed; local image and build cache remain.",
        }},
    };
    std::vector<Trial> trials;
    for (unsigned round = 0; round < options.repeats; ++round) {
        const std::vector<std::string> order = round % 2
            ? std::vector<std::string>{"tuned", "baseline"}
            : std::vector<std::string>{"baseline", "tuned"};
        for (const auto& profile : order) {
            if (interrupted) throw std::runtime_error("Interrupted");
            std::cerr << "Round " << round + 1 << '/' << options.repeats << ": " << profile << '\n';
            auto measured = trial(profile, image, options);
            measured.report["round"] = round + 1;
            report["trials"].push(measured.report);
            trials.push_back(std::move(measured));
            if (!options.output.empty()) write_report(report, options.output, false);
        }
    }
    bool valid = true;
    Json summary = Json::Object{};
    std::map<std::string, double> memories, rates;
    for (const auto* profile : {"baseline", "tuned"}) {
        std::vector<double> rps, p95, memory;
        uint64_t failed = 0;
        for (const auto& measured : trials) {
            if (measured.profile != profile) continue;
            valid = valid && measured.valid;
            rps.push_back(measured.bench.requests_per_second);
            if (measured.bench.p95) p95.push_back(*measured.bench.p95);
            if (measured.memory) memory.push_back(*measured.memory);
            failed += measured.bench.failed;
        }
        rates[profile] = median(rps);
        if (!memory.empty()) memories[profile] = median(memory);
        summary[profile] = Json::Object{
            {"trials", rps.size()},
            {"median_successful_requests_per_second", rates[profile]},
            {"median_p95_ms", p95.empty() ? Json() : Json(median(p95))},
            {"median_load_memory_bytes", memory.empty() ? Json() : Json(memories[profile])},
            {"failed_requests", failed},
        };
    }
    summary["memory_reduction_percent"] = memories.count("baseline") &&
        memories.count("tuned") && memories["baseline"] > 0
            ? Json((1 - memories["tuned"] / memories["baseline"]) * 100) : Json();
    summary["throughput_change_percent"] = rates["baseline"] > 0
        ? Json((rates["tuned"] / rates["baseline"] - 1) * 100) : Json();
    summary["measurement_valid"] = valid;
    summary["interpretation"] = "Descriptive statistics, not significance testing. "
        "Check latency, errors and throughput before claiming a benefit. No energy claim.";
    report["summary"] = summary;
    return {std::move(report), valid};
}

} // namespace opt
