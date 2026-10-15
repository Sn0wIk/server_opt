#include "server_opt.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <regex>
#include <sstream>

namespace opt {
namespace {

std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \r\n\t");
    return first == std::string::npos ? "" :
        text.substr(first, text.find_last_not_of(" \r\n\t") - first + 1);
}

std::string docker(std::vector<std::string> args, double timeout = 30) {
    args.insert(args.begin(), "docker");
    const auto result = run_command(args, timeout, true);
    if (result.exit_code)
        throw std::runtime_error("docker " + args[1] + " failed: " + trim(result.output));
    return trim(result.output);
}

std::string sql(const AnalyzeOptions& o, const std::string& database,
                const std::string& query) {
    return docker({"exec", "-i", o.container, "psql", "-XqAt", "-v", "ON_ERROR_STOP=1",
                   "-U", o.user, "-d", database, "-c", query});
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

Json flags_for(const std::string& query, uint64_t calls, double mean_ms) {
    Json flags = Json::Array{};
    const auto q = lower(query);
    if (q.find("truncate") != std::string::npos)
        flags.push("repeated_truncate_or_ddl_under_load");
    if (q.find("information_schema") != std::string::npos ||
        q.find("pg_catalog") != std::string::npos || q.find("pg_class") != std::string::npos)
        flags.push("schema_or_catalog_introspection");
    if (calls >= 100 && mean_ms < 0.5)
        flags.push("high_frequency_low_latency");
    if (q.find(" like ") != std::string::npos || q.find(" ilike ") != std::string::npos)
        flags.push("like_pattern_may_prevent_index_use");
    if (q.rfind("select", 0) == 0 && q.find(" order by ") != std::string::npos &&
        q.find(" limit ") == std::string::npos)
        flags.push("sorts_without_limit");
    const std::regex comma_join("from\\s+[^()\\s]+\\s+[a-z0-9_\"]+\\s*,",
                                std::regex_constants::icase);
    if (std::regex_search(q, comma_join))
        flags.push("implicit_comma_join_check_plan");
    if (q.rfind("select", 0) == 0 && q.find(" join ") != std::string::npos)
        flags.push("join_check_plan");
    return flags;
}

struct Row {
    uint64_t calls;
    uint64_t rows;
    double total_ms;
    double mean_ms;
    std::string query;
};

} // namespace

Json analyze(const AnalyzeOptions& options) {
    if (options.container.empty() || options.user.empty() || options.database.empty())
        throw std::invalid_argument("analyze requires --container, --user and --database");
    if (options.top < 1 || options.top > 50)
        throw std::invalid_argument("--top must be between 1 and 50");

    if (docker({"inspect", "--format", "{{.State.Running}}", options.container}) != "true")
        throw std::runtime_error("Container is not running: " + options.container);

    Json report = Json::Object{
        {"schema_version", 1}, {"created_at", timestamp()},
        {"target", Json::Object{
            {"container", options.container}, {"database", options.database},
            {"user", options.user},
        }},
        {"scope", "read-only: queries pg_stat_statements; changes nothing"},
        {"postgres_version", sql(options, "postgres", "SELECT version();")},
    };

    const auto extension = sql(options, options.database,
        "SELECT count(*) FROM pg_extension WHERE extname='pg_stat_statements';");
    if (trim(extension) != "1") {
        report["pg_stat_statements"] = "not_enabled";
        report["how_to_enable"] =
            "ALTER SYSTEM SET shared_preload_libraries='pg_stat_statements'; restart; "
            "CREATE EXTENSION pg_stat_statements; then let the real workload run.";
        return report;
    }
    report["pg_stat_statements"] = "enabled";

    const auto raw = sql(options, options.database,
        "SELECT calls, round(total_exec_time::numeric,3), round(mean_exec_time::numeric,6), "
        "rows, left(regexp_replace(query, '[ \\n\\t]+', ' ', 'g'), 400) "
        "FROM pg_stat_statements "
        "WHERE query NOT ILIKE '%pg_stat_statements%' "
        "  AND query NOT ILIKE '%pg_extension%' "
        "ORDER BY total_exec_time DESC LIMIT " + std::to_string(options.top) + ";");

    std::vector<Row> rows;
    std::istringstream lines(raw);
    std::string line;
    double listed_ms = 0;
    while (std::getline(lines, line)) {
        if (line.empty()) continue;
        const auto p1 = line.find('|');
        if (p1 == std::string::npos) continue;
        const auto p2 = line.find('|', p1 + 1);
        const auto p3 = line.find('|', p2 + 1);
        const auto p4 = line.find('|', p3 + 1);
        if (p2 == std::string::npos || p3 == std::string::npos || p4 == std::string::npos)
            continue;
        Row row{
            std::stoull(line.substr(0, p1)),
            std::stoull(line.substr(p3 + 1, p4 - p3 - 1)),
            std::stod(line.substr(p1 + 1, p2 - p1 - 1)),
            std::stod(line.substr(p2 + 1, p3 - p2 - 1)),
            trim(line.substr(p4 + 1)),
        };
        listed_ms += row.total_ms;
        rows.push_back(std::move(row));
    }

    Json top = Json::Array{};
    size_t rank = 0;
    for (const auto& row : rows) {
        ++rank;
        top.push(Json::Object{
            {"rank", rank}, {"calls", row.calls}, {"total_ms", row.total_ms},
            {"mean_ms", row.mean_ms}, {"rows", row.rows},
            {"share_of_listed_time_pct",
             listed_ms > 0 ? Json(row.total_ms / listed_ms * 100) : Json()},
            {"flags", flags_for(row.query, row.calls, row.mean_ms)},
            {"query", row.query.substr(0, 400)},
        });
    }
    report["listed_total_ms"] = listed_ms;
    report["top_queries_by_total_time"] = top;
    report["notes"] = Json::Array{
        "pg_stat_statements normalizes literals to $1..$n; EXPLAIN needs concrete values, "
        "so plans are not fetched automatically.",
        "Ranking is cumulative time: frequent cheap queries and rare heavy queries both "
        "surface. Check flags and calls, not only mean_ms.",
        "Stats cover since the extension was enabled or last reset.",
    };
    return report;
}

} // namespace opt
