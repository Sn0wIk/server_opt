#pragma once

#include "json.hpp"
#include <chrono>
#include <csignal>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace opt {

inline constexpr size_t max_body = 1024 * 1024;
inline constexpr size_t max_samples = 100000;
inline const std::string payload = "{\"ok\":true,\"payload\":\"server-opt\"}\n";
extern volatile std::sig_atomic_t interrupted;

struct CurlRuntime {
    CurlRuntime();
    ~CurlRuntime();
    CurlRuntime(const CurlRuntime&) = delete;
    CurlRuntime& operator=(const CurlRuntime&) = delete;
};

struct BenchOptions {
    std::string url;
    double duration = 10;
    unsigned concurrency = 8;
    double timeout = 3;
    long expected_status = 200;
    std::optional<std::string> expected_body;
};

struct BenchResult {
    Json report;
    uint64_t attempts = 0;
    uint64_t successful = 0;
    uint64_t failed = 0;
    double requests_per_second = 0;
    std::optional<double> p95;
};

void validate(const BenchOptions& options);
BenchResult benchmark(const BenchOptions& options);

struct CommandResult {
    int exit_code;
    std::string output;
};
CommandResult run_command(const std::vector<std::string>& argv, double timeout = 60,
                          bool honor_interrupt = true);
std::string read_file(const std::filesystem::path& path, size_t limit = max_body);
std::string timestamp();
void write_report(const Json& report, const std::filesystem::path& output = {},
                  bool print = true);
Json audit();
Json parse_meminfo(const std::string& text);
double memory_bytes(const std::string& docker_memory);
double median(std::vector<double> values);

struct ExperimentOptions {
    std::filesystem::path project_root;
    std::filesystem::path output;
    double duration = 15;
    unsigned concurrency = 8;
    unsigned repeats = 3;
    double cpus = 1;
    std::string memory = "256m";
};
struct ExperimentResult {
    Json report;
    bool valid;
};
ExperimentResult experiment(const ExperimentOptions& options);

struct PostgresOptions {
    std::filesystem::path project_root;
    std::filesystem::path output;
    unsigned rows = 1000000;
    unsigned clients = 8;
    unsigned duration = 10;
    unsigned repeats = 4;
    unsigned inserts_per_client = 1000;
};
struct PgbenchStats {
    uint64_t successful;
    uint64_t failed;
    double transactions_per_second;
    double average_latency_ms;
};
PgbenchStats parse_pgbench(const std::string& text);
ExperimentResult postgres_experiment(const PostgresOptions& options);

struct AnalyzeOptions {
    std::string container;
    std::string database;
    std::string user;
    unsigned top = 15;
};
Json analyze(const AnalyzeOptions& options);

} // namespace opt
