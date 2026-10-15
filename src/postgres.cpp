#include "server_opt.hpp"

#include <algorithm>
#include <atomic>
#include <iostream>
#include <regex>
#include <thread>
#include <unistd.h>

namespace opt {
namespace {
using Clock = std::chrono::steady_clock;

std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \r\n\t");
    return first == std::string::npos ? "" :
        text.substr(first, text.find_last_not_of(" \r\n\t") - first + 1);
}

std::string docker(std::vector<std::string> args, double timeout = 60,
                   bool honor_interrupt = true) {
    args.insert(args.begin(), "docker");
    const auto result = run_command(args, timeout, honor_interrupt);
    if (result.exit_code)
        throw std::runtime_error("docker " + args[1] + " failed: " + result.output);
    return trim(result.output);
}

struct Cleanup {
    std::vector<std::vector<std::string>> commands;
    ~Cleanup() {
        for (auto command = commands.rbegin(); command != commands.rend(); ++command) {
            try { docker(*command, 30, false); }
            catch (...) { std::cerr << "Cleanup failed for " << command->back() << '\n'; }
        }
    }
};

std::string sql(const std::string& server, const std::string& database,
                const std::string& query, double timeout = 120) {
    return docker({"exec", server, "psql", "-XqAt", "-v", "ON_ERROR_STOP=1",
                   "-U", "postgres", "-d", database, "-c", query}, timeout);
}

std::string sql_file(const std::string& server, const std::string& database,
                     const std::string& path, unsigned rows = 0) {
    auto args = std::vector<std::string>{
        "exec", server, "psql", "-XqAt", "-v", "ON_ERROR_STOP=1", "-U", "postgres",
        "-d", database, "-f", path,
    };
    if (rows) { args.push_back("-v"); args.push_back("rows=" + std::to_string(rows)); }
    return docker(args, 600);
}

std::string fingerprint(const std::string& server, const std::string& database) {
    const auto result = sql_file(server, database, "/bench/verify.sql");
    if (!std::regex_match(result, std::regex("[0-9a-f]{32}")))
        throw std::runtime_error("Unexpected verification fingerprint: " + result);
    return result;
}

uint64_t cpu_microseconds(const std::string& server) {
    std::istringstream fields(docker({"exec", server, "cat", "/sys/fs/cgroup/cpu.stat"}));
    std::string key;
    uint64_t amount = 0;
    while (fields >> key >> amount) if (key == "usage_usec") return amount;
    throw std::runtime_error("CPU accounting requires cgroup v2 usage_usec");
}

struct Size {
    uint64_t heap, indexes, total;
    Json json() const {
        return Json::Object{{"heap_bytes", heap}, {"indexes_bytes", indexes}, {"total_bytes", total}};
    }
};

Size relation_size(const std::string& server, const std::string& database) {
    auto text = sql(server, database,
        "SELECT pg_relation_size('products'),pg_indexes_size('products'),"
        "pg_total_relation_size('products');");
    std::replace(text.begin(), text.end(), '|', ' ');
    std::istringstream values(text);
    Size size{};
    if (!(values >> size.heap >> size.indexes >> size.total))
        throw std::runtime_error("Could not read PostgreSQL relation sizes");
    return size;
}

struct Measurement {
    std::string profile, workload;
    PgbenchStats stats;
    double server_cpu_us_per_transaction;
    bool valid;
    Json report;
};

Measurement measure(const std::string& profile, const std::string& workload,
                    const std::string& database, unsigned round,
                    const std::string& image, const std::string& server,
                    const std::string& network, const PostgresOptions& options,
                    unsigned seconds = 0, bool record_metrics = true) {
    static unsigned serial = 0;
    const auto client = server + "-client-" + std::to_string(++serial);
    Cleanup cleanup;
    auto args = std::vector<std::string>{
        "create", "--name", client, "--label", "app=server-opt-postgres",
        "--network", network, "--cpus", "1", "--memory", "256m", "--memory-swap", "256m",
        "--read-only", "--cap-drop", "ALL", "--security-opt", "no-new-privileges",
        "--user", "postgres", "--env", "LC_ALL=C",
        "--env", "PGOPTIONS=-c statement_timeout=60000 -c default_transaction_read_only=" +
            std::string(workload == "read" ? "on" : "off"),
        "--entrypoint", "pgbench", image, "--host", server, "--username", "postgres",
        "--no-vacuum", "--protocol", "prepared", "--client", std::to_string(options.clients),
        "--jobs", "2", "--random-seed", "20261015", "--progress", "1",
        "--file", workload == "read" ? "/bench/search.sql" : "/bench/insert.sql",
    };
    if (workload == "read") {
        args.push_back("--time");
        args.push_back(std::to_string(seconds ? seconds : options.duration));
    } else {
        args.push_back("--transactions");
        args.push_back(std::to_string(options.inserts_per_client));
    }
    args.push_back(database);
    docker(args);
    cleanup.commands.push_back({"rm", "--force", "--volumes", client});
    const uint64_t cpu_before = record_metrics ? cpu_microseconds(server) : 0;
    Json samples = Json::Array{};
    Json sample_errors = Json::Array{};
    std::atomic<bool> stop{false};
    bool sampling_failed = false;
    unsigned sample_count = 0;
    std::thread sampler;
    const auto start = Clock::now();
    // Start before requesting stats: Docker can wait indefinitely for a stats
    // stream on a container that is still in the "created" state.
    docker({"start", client});
    if (record_metrics) sampler = std::thread([&] {
        while (!stop.load() && !interrupted) {
            try {
                const auto snapshot = docker({
                    "stats", "--no-stream", "--format",
                    "{{.Name}}\t{{.CPUPerc}}\t{{.MemUsage}}", server, client,
                }, 15);
                samples.push(Json::Object{{"at", timestamp()}, {"name_cpu_memory", snapshot}});
                ++sample_count;
            } catch (const std::exception& error) {
                sampling_failed = true;
                sample_errors.push(error.what());
            }
            for (int i = 0; i < 10 && !stop.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
    std::string output;
    try {
        docker({"wait", client}, options.duration + 600);
        output = docker({"logs", client});
    }
    catch (...) {
        stop.store(true);
        if (sampler.joinable()) sampler.join();
        throw;
    }
    const auto end = Clock::now();
    stop.store(true);
    if (sampler.joinable()) sampler.join();
    const auto cpu_after = record_metrics ? cpu_microseconds(server) : 0;
    const auto exit_code = docker({"inspect", "--format", "{{.State.ExitCode}}", client});
    const auto state = docker({"inspect", "--format",
                               "{{.State.Running}} {{.State.OOMKilled}}", server});
    if (exit_code != "0")
        throw std::runtime_error("pgbench failed (exit " + exit_code + "): " + output);
    const auto stats = parse_pgbench(output);
    const bool count_ok = workload == "read" ||
        stats.successful == uint64_t(options.clients) * options.inserts_per_client;
    const bool valid = stats.successful > 0 && stats.failed == 0 && count_ok &&
        state == "true false" && !sampling_failed && (!record_metrics || sample_count);
    const double cpu = stats.successful ? double(cpu_after - cpu_before) / stats.successful : 0;
    Json report = Json::Object{
        {"profile", profile}, {"workload", workload}, {"round", round},
        {"successful_transactions", stats.successful}, {"failed_transactions", stats.failed},
        {"transactions_per_second", stats.transactions_per_second},
        {"average_latency_ms", stats.average_latency_ms},
        {"server_cpu_us_per_transaction", cpu},
        {"wall_seconds_including_container_start", std::chrono::duration<double>(end - start).count()},
        {"pgbench_output", output}, {"docker_stats", samples}, {"sampling_errors", sample_errors},
        {"measurement_valid", valid},
    };
    return {profile, workload, stats, cpu, valid, std::move(report)};
}
} // namespace

PgbenchStats parse_pgbench(const std::string& text) {
    auto extract = [&](const std::string& expression) {
        std::smatch match;
        if (!std::regex_search(text, match, std::regex(expression)))
            throw std::runtime_error("Incomplete pgbench report, missing: " + expression);
        return match[1].str();
    };
    PgbenchStats stats{
        std::stoull(extract(R"(number of transactions actually processed:\s*([0-9]+))")),
        std::stoull(extract(R"(number of failed transactions:\s*([0-9]+))")),
        std::stod(extract(R"((?:^|\n)tps = ([0-9]+(?:\.[0-9]+)?))")),
        std::stod(extract(R"(latency average = ([0-9]+(?:\.[0-9]+)?) ms)")),
    };
    if (!std::isfinite(stats.transactions_per_second) || !std::isfinite(stats.average_latency_ms))
        throw std::runtime_error("Invalid pgbench measurement");
    return stats;
}

ExperimentResult postgres_experiment(const PostgresOptions& options) {
    if (options.rows < 10000 || options.rows > 10000000)
        throw std::invalid_argument("rows must be between 10000 and 10000000");
    if (!options.clients || options.clients > 32)
        throw std::invalid_argument("clients must be between 1 and 32");
    if (options.duration < 3 || options.duration > 300 ||
        options.repeats < 2 || options.repeats > 10 ||
        options.inserts_per_client < 100 || options.inserts_per_client > 10000)
        throw std::invalid_argument("duration: 3..300; repeats: 2..10; inserts-per-client: 100..10000");
    const auto directory = options.project_root / "postgres";
    for (const auto* file : {"Dockerfile", "setup.sql", "search.sql", "insert.sql", "verify.sql"})
        if (!std::filesystem::is_regular_file(directory / file))
            throw std::runtime_error("Missing postgres experiment file: " + std::string(file));
    const auto environment = docker({"info", "--format",
        "{{.OSType}}\t{{.OperatingSystem}}\t{{.Architecture}}\t{{.NCPU}}\t"
        "{{.MemTotal}}\t{{.CgroupVersion}}"});
    if (environment.rfind("linux\t", 0) != 0 || environment.back() != '2')
        throw std::runtime_error("Postgres experiment needs Linux Docker with cgroup v2");

    std::cerr << "Building PostgreSQL experiment image…\n";
    docker({"build", "--tag", "server-opt-postgres:local", directory.string()}, 900);
    const auto image = docker({"image", "inspect", "--format", "{{.Id}}", "server-opt-postgres:local"});
    const auto suffix = std::to_string(getpid()) + "-" +
                        std::to_string(Clock::now().time_since_epoch().count());
    const auto server = "server-opt-pg-" + suffix;
    const auto network = server + "-net";
    const auto volume = server + "-data";
    Cleanup cleanup;
    docker({"network", "create", "--internal", "--label", "app=server-opt-postgres", network});
    cleanup.commands.push_back({"network", "rm", network});
    docker({"volume", "create", "--label", "app=server-opt-postgres", volume});
    cleanup.commands.push_back({"volume", "rm", volume});
    docker({"run", "--detach", "--name", server, "--label", "app=server-opt-postgres",
            "--network", network, "--cpus", "2", "--memory", "1g", "--memory-swap", "1g",
            "--shm-size", "256m", "--mount", "type=volume,src=" + volume + ",dst=/var/lib/postgresql/data",
            "--env", "POSTGRES_HOST_AUTH_METHOD=trust", image,
            "postgres", "-c", "shared_buffers=256MB", "-c", "jit=off",
            "-c", "max_parallel_workers_per_gather=0"});
    cleanup.commands.push_back({"rm", "--force", server});
    bool ready = false;
    for (unsigned attempt = 0; attempt < 90 && !interrupted; ++attempt) {
        const auto probe = run_command({"docker", "exec", server, "pg_isready",
                                       "-h", "127.0.0.1", "-U", "postgres"}, 5);
        if (probe.exit_code == 0) { ready = true; break; }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (!ready) throw std::runtime_error("Postgres not ready: " + docker({"logs", server}));
    std::cerr << "Creating deterministic catalogue: " << options.rows << " products…\n";
    sql(server, "postgres", "CREATE DATABASE baseline;");
    sql_file(server, "baseline", "/bench/setup.sql", options.rows);
    sql(server, "postgres", "CREATE DATABASE tuned TEMPLATE baseline;");
    const std::string index_sql =
        "CREATE INDEX products_search_idx ON products (category_id, price, id) "
        "INCLUDE (name) WHERE in_stock;";
    const auto index_start = Clock::now();
    sql(server, "tuned", index_sql, 600);
    const double index_seconds = std::chrono::duration<double>(Clock::now() - index_start).count();
    sql(server, "tuned", "VACUUM (ANALYZE) products;");
    const auto base_size = relation_size(server, "baseline");
    const auto tuned_size = relation_size(server, "tuned");
    const auto expected = fingerprint(server, "baseline");
    const auto actual = fingerprint(server, "tuned");
    if (expected != actual) throw std::runtime_error("Search outputs differ after indexing");
    const auto explain = "EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON) "
        "SELECT id,name,price FROM products WHERE category_id=42 AND in_stock AND price>=500 "
        "ORDER BY price,id LIMIT 20;";
    Json report = Json::Object{
        {"schema_version", 1}, {"created_at", timestamp()},
        {"experiment", "Product search: category index versus additional partial covering index"},
        {"docker_environment", environment}, {"image_id", image},
        {"postgres_version", sql(server, "postgres", "SELECT version();")},
        {"durability_fsync_synccommit_fullpagewrites",
         sql(server, "postgres", "SELECT current_setting('fsync'),current_setting('synchronous_commit'),"
                                 "current_setting('full_page_writes');")},
        {"settings", Json::Object{
            {"rows", options.rows}, {"clients", options.clients}, {"pgbench_threads", 2},
            {"duration_seconds", options.duration}, {"repeats", options.repeats},
            {"inserts_per_client", options.inserts_per_client},
            {"server_cpus", 2}, {"server_memory_mib", 1024},
            {"client_cpus", 1}, {"client_memory_mib", 256},
        }},
        {"setup_sql", read_file(directory / "setup.sql")},
        {"search_script", read_file(directory / "search.sql")},
        {"insert_script", read_file(directory / "insert.sql")},
        {"verification_sql", read_file(directory / "verify.sql")},
        {"added_index_sql", index_sql},
        {"index_build_wall_seconds", index_seconds},
        {"size_baseline", base_size.json()}, {"size_tuned", tuned_size.json()},
        {"verification_before", Json::Object{
            {"baseline", expected}, {"tuned", actual}, {"probe_queries", 300}, {"equal", true},
        }},
        {"explain_baseline_json", sql(server, "baseline", explain)},
        {"explain_tuned_json", sql(server, "tuned", explain)},
        {"trials", Json::Array{}},
        {"limitations", Json::Array{
            "Synthetic catalogue; SQL/database layer only, not full HTTP API latency.",
            "Standard PostgreSQL indexing, not a new database engine or general server optimizer.",
            "Warm-cache reads, eight clients by default. No maximum-capacity or SLA claim.",
            "Read correctness compares 300 ordered result sets before and after all read trials.",
            "Write test covers inserts only; each trial starts from a fresh database clone.",
            "Server CPU includes background work and accounting commands; not energy.",
            "Load generator and database share a VM/host; inspect CPU samples for bottlenecks.",
            "Mean SQL transaction latency; no p95/p99 measurement.",
            "Temporary containers, data volume and network are removed; image/cache remain.",
        }},
    };
    auto save = [&] { if (!options.output.empty()) write_report(report, options.output, false); };
    save();
    std::vector<Measurement> measurements;
    for (unsigned round = 1; round <= options.repeats; ++round) {
        const auto order = round % 2 ? std::vector<std::string>{"baseline", "tuned"}
                                     : std::vector<std::string>{"tuned", "baseline"};
        for (const auto& profile : order) {
            std::cerr << "Read round " << round << '/' << options.repeats << ": " << profile << '\n';
            const auto warmup = measure(profile, "read", profile, round, image, server,
                                        network, options, 2, false);
            if (!warmup.valid) throw std::runtime_error("Read warm-up failed");
            auto result = measure(profile, "read", profile, round, image, server, network, options);
            report["trials"].push(result.report);
            measurements.push_back(std::move(result));
            save();
        }
    }
    const auto base_after = fingerprint(server, "baseline");
    const auto tuned_after = fingerprint(server, "tuned");
    const bool unchanged = base_after == expected && tuned_after == expected;
    report["verification_after_reads"] = Json::Object{
        {"baseline", base_after}, {"tuned", tuned_after}, {"equal_and_unchanged", unchanged},
    };
    if (!unchanged) { save(); throw std::runtime_error("Read results changed during experiment"); }

    for (unsigned round = 1; round <= options.repeats; ++round) {
        const auto order = round % 2 ? std::vector<std::string>{"tuned", "baseline"}
                                     : std::vector<std::string>{"baseline", "tuned"};
        for (const auto& profile : order) {
            std::cerr << "Insert round " << round << '/' << options.repeats << ": " << profile << '\n';
            sql(server, "postgres", "CREATE DATABASE write_trial TEMPLATE " + profile + ";");
            // Give both clones the same catalogue search warm-up before measuring inserts.
            const auto warmup = measure(profile, "read", "write_trial", round, image, server,
                                        network, options, 2, false);
            if (!warmup.valid) throw std::runtime_error("Insert warm-up failed");
            auto result = measure(profile, "insert", "write_trial", round, image, server, network, options);
            const auto count = std::stoull(sql(server, "write_trial", "SELECT count(*) FROM products;"));
            result.valid = result.valid &&
                count == uint64_t(options.rows) + uint64_t(options.clients) * options.inserts_per_client;
            result.report["rows_after"] = count;
            result.report["measurement_valid"] = result.valid;
            result.report["size_after"] = relation_size(server, "write_trial").json();
            report["trials"].push(result.report);
            measurements.push_back(std::move(result));
            save();
            sql(server, "postgres", "DROP DATABASE write_trial;");
        }
    }
    bool valid = unchanged;
    Json summary = Json::Object{};
    for (const auto* workload : {"read", "insert"}) {
        Json group = Json::Object{};
        std::map<std::string, double> rates, latencies;
        for (const auto* profile : {"baseline", "tuned"}) {
            std::vector<double> tps, latency, cpu;
            uint64_t failed = 0;
            for (const auto& trial : measurements) {
                if (trial.profile != profile || trial.workload != workload) continue;
                valid = valid && trial.valid;
                tps.push_back(trial.stats.transactions_per_second);
                latency.push_back(trial.stats.average_latency_ms);
                cpu.push_back(trial.server_cpu_us_per_transaction);
                failed += trial.stats.failed;
            }
            rates[profile] = median(tps);
            latencies[profile] = median(latency);
            group[profile] = Json::Object{
                {"median_transactions_per_second", rates[profile]},
                {"median_average_latency_ms", latencies[profile]},
                {"median_server_cpu_us_per_transaction", median(cpu)},
                {"min_transactions_per_second", *std::min_element(tps.begin(), tps.end())},
                {"max_transactions_per_second", *std::max_element(tps.begin(), tps.end())},
                {"failed_transactions", failed}, {"trials", tps.size()},
            };
        }
        group["throughput_change_percent"] = rates["baseline"] > 0
            ? Json((rates["tuned"] / rates["baseline"] - 1) * 100) : Json();
        group["latency_reduction_percent"] = latencies["baseline"] > 0
            ? Json((1 - latencies["tuned"] / latencies["baseline"]) * 100) : Json();
        summary[workload] = group;
    }
    summary["extra_relation_bytes"] = tuned_size.total - base_size.total;
    summary["relation_size_increase_percent"] = (double(tuned_size.total) / base_size.total - 1) * 100;
    summary["measurement_valid"] = valid;
    summary["interpretation"] =
        "Compare both read benefit and insert/storage costs. Descriptive medians, not "
        "statistical significance or a universal gain. No energy or whole-API claim.";
    report["summary"] = summary;
    save();
    return {std::move(report), valid};
}
} // namespace opt
