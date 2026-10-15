#include "server_opt.hpp"

#include <iostream>
#include <set>

namespace {
void on_signal(int) { opt::interrupted = 1; }

void help() {
    std::cout <<
        "server-opt — C++17 resource audit, Nginx and PostgreSQL experiments\n\n"
        "  server-opt audit [--output FILE]\n"
        "  server-opt bench URL [--duration SEC] [--concurrency N] [--timeout SEC]\n"
        "                   [--expected-status CODE] [--expect-body FILE] [--output FILE]\n"
        "  server-opt experiment [--duration SEC] [--concurrency N] [--repeats N]\n"
        "                        [--cpus N] [--memory 256m] [--project-root DIR] [--output FILE]\n"
        "  server-opt postgres [--rows N] [--clients N] [--duration SEC] [--repeats N]\n"
        "                      [--inserts-per-client N] [--project-root DIR] [--output FILE]\n"
        "  server-opt analyze --container NAME --database DB --user USER [--top N]\n"
        "                     [--output FILE]\n\n"
        "Defaults: bench 10 seconds, 8 connections, 3 second timeout, HTTP 200.\n"
        "Experiment: 15 seconds, 3 repetitions per profile, 1 CPU, 256 MiB.\n"
        "Postgres: 1 million rows, 8 clients, 10 seconds, 4 repetitions, 1000 inserts/client.\n"
        "Audit changes no settings. Bench sends load: target your own test service.\n"
        "Experiments require Docker with Linux containers. Postgres creates only disposable data.\n"
        "Analyze reads pg_stat_statements of a running PostgreSQL container; read-only.\n";
}

double number(const std::string& text) {
    size_t consumed = 0;
    const double value = std::stod(text, &consumed);
    if (consumed != text.size() || !std::isfinite(value))
        throw std::invalid_argument("Invalid number: " + text);
    return value;
}

unsigned integer(const std::string& text) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("Expected a nonnegative integer: " + text);
    const auto value = std::stoull(text);
    if (value > 10000000) throw std::invalid_argument("Integer out of range: " + text);
    return static_cast<unsigned>(value);
}
} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);
    try {
        if (argc < 2) { help(); return 1; }
        const std::string command = argv[1];
        if (command == "--help" || command == "-h") { help(); return 0; }
        if (command != "audit" && command != "bench" && command != "experiment" &&
            command != "postgres" && command != "analyze")
            throw std::invalid_argument("Unknown command: " + command);
        opt::CurlRuntime curl;
        opt::BenchOptions bench;
        opt::ExperimentOptions experiment;
        experiment.project_root = std::filesystem::current_path();
        opt::PostgresOptions postgres;
        postgres.project_root = experiment.project_root;
        opt::AnalyzeOptions analyze;
        std::filesystem::path output;
        int begin = 2;
        if (command == "bench") {
            if (argc < 3) throw std::invalid_argument("bench requires a URL");
            if (std::string(argv[2]) == "--help") { help(); return 0; }
            bench.url = argv[2];
            begin = 3;
        }
        std::set<std::string> seen;
        for (int i = begin; i < argc; ++i) {
            const std::string flag = argv[i];
            if (flag == "--help") { help(); return 0; }
            if (!seen.insert(flag).second)
                throw std::invalid_argument("Duplicate option: " + flag);
            if (i + 1 >= argc) throw std::invalid_argument("Missing value for " + flag);
            const std::string value = argv[++i];
            if (flag == "--output") output = value;
            else if (command == "postgres" && flag == "--duration") postgres.duration = integer(value);
            else if (command == "postgres" && flag == "--rows") postgres.rows = integer(value);
            else if (command == "postgres" && flag == "--clients") postgres.clients = integer(value);
            else if (command == "postgres" && flag == "--repeats") postgres.repeats = integer(value);
            else if (command == "postgres" && flag == "--inserts-per-client")
                postgres.inserts_per_client = integer(value);
            else if (command == "postgres" && flag == "--project-root") postgres.project_root = value;
            else if (command == "analyze" && flag == "--container") analyze.container = value;
            else if (command == "analyze" && flag == "--database") analyze.database = value;
            else if (command == "analyze" && flag == "--user") analyze.user = value;
            else if (command == "analyze" && flag == "--top") analyze.top = integer(value);
            else if ((command == "bench" || command == "experiment") && flag == "--duration")
                bench.duration = experiment.duration = number(value);
            else if ((command == "bench" || command == "experiment") && flag == "--concurrency")
                bench.concurrency = experiment.concurrency = integer(value);
            else if (command == "bench" && flag == "--timeout") bench.timeout = number(value);
            else if (command == "bench" && flag == "--expected-status")
                bench.expected_status = integer(value);
            else if (command == "bench" && flag == "--expect-body")
                bench.expected_body = opt::read_file(value);
            else if (command == "experiment" && flag == "--repeats")
                experiment.repeats = integer(value);
            else if (command == "experiment" && flag == "--cpus") experiment.cpus = number(value);
            else if (command == "experiment" && flag == "--memory") experiment.memory = value;
            else if (command == "experiment" && flag == "--project-root")
                experiment.project_root = value;
            else throw std::invalid_argument("Unknown option for " + command + ": " + flag);
        }
        if (command == "experiment" && output.empty())
            output = experiment.project_root / "results/experiment.json";
        if (command == "postgres" && output.empty())
            output = postgres.project_root / "results/postgres.json";
        if (command == "analyze" && output.empty())
            output = experiment.project_root / "results/analyze.json";
        if (!output.empty() && !output.parent_path().empty() &&
            !std::filesystem::is_directory(output.parent_path()))
            throw std::invalid_argument("Output directory does not exist");
        if (command == "audit") {
            opt::write_report(opt::audit(), output);
        } else if (command == "bench") {
            const auto result = opt::benchmark(bench);
            opt::write_report(result.report, output);
            if (opt::interrupted) return 130;
            if (!result.successful || result.failed) return 2;
        } else if (command == "analyze") {
            opt::write_report(opt::analyze(analyze), output);
        } else if (command == "postgres") {
            postgres.output = output;
            const auto result = opt::postgres_experiment(postgres);
            opt::write_report(result.report, output);
            if (!result.valid) return 2;
        } else {
            experiment.output = output;
            const auto result = opt::experiment(experiment);
            opt::write_report(result.report, output);
            if (!result.valid) return 2;
        }
        return opt::interrupted ? 130 : 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return opt::interrupted ? 130 : 1;
    }
}
