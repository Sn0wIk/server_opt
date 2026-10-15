#include "server_opt.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <functional>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template<typename Function>
void rejects(Function function) {
    bool rejected = false;
    try { function(); } catch (const std::exception&) { rejected = true; }
    require(rejected, "Expected rejection");
}

// Test fixture only; the server under measurement in the experiment is Nginx.
class HttpFixture {
public:
    HttpFixture() {
        listener_ = socket(AF_INET, SOCK_STREAM, 0);
        require(listener_ >= 0, "socket failed");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(listener_, 32) != 0) {
            close(listener_);
            throw std::runtime_error("bind/listen failed");
        }
        socklen_t size = sizeof(address);
        getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size);
        url = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port));
        acceptor_ = std::thread([this] {
            while (!stop_.load()) {
                pollfd descriptor{listener_, POLLIN, 0};
                if (poll(&descriptor, 1, 30) <= 0 || !(descriptor.revents & POLLIN)) continue;
                const int client = accept(listener_, nullptr, nullptr);
                if (client < 0) continue;
                ++connections;
                clients_.emplace_back([this, client] { serve(client); close(client); });
            }
        });
    }
    ~HttpFixture() {
        stop_.store(true);
        acceptor_.join();
        close(listener_);
        for (auto& client : clients_) client.join();
    }
    std::string url;
    std::atomic<unsigned> connections{0};

private:
    int listener_;
    std::atomic<bool> stop_{false};
    std::thread acceptor_;
    std::vector<std::thread> clients_;

    static bool send_all(int fd, const std::string& data) {
        size_t position = 0;
        while (position < data.size()) {
            const auto sent = send(fd, data.data() + position, data.size() - position, 0);
            if (sent < 0 && errno == EINTR) continue;
            if (sent <= 0) return false;
            position += static_cast<size_t>(sent);
        }
        return true;
    }

    void serve(int client) {
        timeval timeout{0, 200000};
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        std::string buffer;
        while (!stop_.load()) {
            pollfd descriptor{client, POLLIN, 0};
            if (poll(&descriptor, 1, 30) <= 0) continue;
            char chunk[4096];
            const auto bytes = recv(client, chunk, sizeof(chunk), 0);
            if (bytes <= 0) return;
            buffer.append(chunk, static_cast<size_t>(bytes));
            const auto end = buffer.find("\r\n\r\n");
            if (end == std::string::npos) continue;
            const auto first = buffer.find(' ');
            const auto last = buffer.find(' ', first + 1);
            const auto path = buffer.substr(first + 1, last - first - 1);
            buffer.erase(0, end + 4);
            if (path == "/close") return;
            if (path == "/slow") std::this_thread::sleep_for(std::chrono::milliseconds(120));
            if (path == "/chunked") {
                if (!send_all(client, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                                      "3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n")) return;
                continue;
            }
            std::string body = path == "/wrong" ? "incorrect" : opt::payload;
            if (path == "/large") body.assign(opt::max_body + 1, 'x');
            if (path == "/empty") body.clear();
            const int status = path == "/error" ? 503 : path == "/redirect" ? 302 : 200;
            const auto advertised = path == "/truncated" ? body.size() + 100 : body.size();
            std::string headers = "HTTP/1.1 " + std::to_string(status) + " Test\r\n";
            if (path == "/redirect") headers += "Location: /ok\r\n";
            headers += "Content-Length: " + std::to_string(advertised) + "\r\n\r\n";
            if (!send_all(client, headers + body)) return;
            if (path == "/truncated") return;
        }
    }
};

opt::BenchResult measure(HttpFixture& server, const std::string& path,
                         std::optional<std::string> body = opt::payload, double timeout = 0.2) {
    opt::BenchOptions options;
    options.url = server.url + path;
    options.duration = 0.08;
    options.concurrency = 2;
    options.timeout = timeout;
    options.expected_body = std::move(body);
    return opt::benchmark(options);
}
} // namespace

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);
    if (argc > 1 && std::string(argv[1]) == "--echo-child") {
        std::cout << argv[2];
        return 7;
    }
    if (argc > 1 && std::string(argv[1]) == "--sleep-child") {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        return 0;
    }
    try {
        opt::CurlRuntime runtime;
        HttpFixture server;
        unsigned passed = 0;
        auto test = [&](const char* name, const std::function<void()>& function) {
            function();
            ++passed;
            std::cout << "PASS " << name << '\n';
        };
        test("successful responses and connection reuse", [&] {
            const auto before = server.connections.load();
            const auto result = measure(server, "/ok");
            require(result.successful > 10 && result.failed == 0, "Success count incorrect");
            require(result.successful == result.attempts, "Attempt count incorrect");
            require(result.p95 && *result.p95 >= 0, "Missing latency");
            require(server.connections.load() - before <= 2, "Connections were not reused");
        });
        test("HTTP failures are excluded from throughput", [&] {
            const auto result = measure(server, "/error");
            require(result.attempts > 0 && result.failed == result.attempts, "Error not counted");
            require(result.requests_per_second == 0 && !result.p95, "Failed response counted as success");
        });
        test("HTTP 200 with wrong body fails validation", [&] {
            const auto result = measure(server, "/wrong");
            require(!result.successful && result.failed > 0, "Wrong body accepted");
            require(result.report.dump().find("body_mismatch") != std::string::npos,
                    "Missing body error");
        });
        test("timeout fails instead of hanging", [&] {
            const auto result = measure(server, "/slow", opt::payload, 0.02);
            require(!result.successful && result.failed > 0, "Timeout accepted");
        });
        test("oversized body is bounded and rejected", [&] {
            const auto result = measure(server, "/large");
            require(!result.successful && result.failed > 0, "Oversized body accepted");
            require(result.report.dump().find("body_exceeds_1_mib") != std::string::npos,
                    "Missing size error");
        });
        test("chunked responses are decoded before exact comparison", [&] {
            const auto result = measure(server, "/chunked", "abcde");
            require(result.successful > 0 && !result.failed, "Chunked response failed");
        });
        test("empty expected body remains a validation requirement", [&] {
            require(measure(server, "/empty", "").successful > 0, "Empty body failed");
            require(measure(server, "/ok", "").successful == 0, "Nonempty body accepted");
        });
        test("redirects are not followed", [&] {
            require(measure(server, "/redirect").successful == 0, "Redirect followed");
        });
        test("truncated response is not a success", [&] {
            const auto result = measure(server, "/truncated");
            require(!result.successful && result.failed > 0, "Truncated body accepted");
        });
        test("closed connection is a failure", [&] {
            const auto result = measure(server, "/close");
            require(!result.successful && result.failed > 0, "Closed connection accepted");
        });
        test("invalid parameters rejected before requests", [&] {
            for (const auto* url : {"ftp://localhost", "http://user:pass@localhost/",
                                    "http://localhost:99999", "http://localhost/#x",
                                    "http://localhost/with space"}) {
                opt::BenchOptions options;
                options.url = url;
                rejects([&] { opt::benchmark(options); });
            }
            opt::BenchOptions options;
            options.url = server.url;
            options.concurrency = 0;
            rejects([&] { opt::benchmark(options); });
            options.concurrency = 1;
            options.duration = std::nan("");
            rejects([&] { opt::benchmark(options); });
            options.duration = 0.1;
            options.timeout = 0;
            rejects([&] { opt::benchmark(options); });
        });
        test("Linux memory uses available including reclaimable pages", [&] {
            const auto memory = opt::parse_meminfo(
                "MemTotal: 1000 kB\nMemFree: 100 kB\nMemAvailable: 600 kB\n"
                "SwapTotal: 200 kB\nSwapFree: 150 kB\n");
            require(memory.at("used_excluding_reclaimable_bytes").dump() == "409600",
                    "Incorrect used memory");
            require(opt::parse_meminfo("MemTotal: 1000 kB").at(
                        "used_excluding_reclaimable_bytes").dump() == "null",
                    "Missing memory was fabricated");
        });
        test("Docker memory units", [&] {
            require(opt::memory_bytes("12.5MiB / 256MiB") == 12.5 * 1048576, "MiB incorrect");
            require(opt::memory_bytes("20MB / 1GB") == 20000000, "MB incorrect");
            rejects([] { opt::memory_bytes("unavailable"); });
        });
        test("JSON escaping and non-finite numbers", [&] {
            require(opt::Json("a\n\"\\\t").dump() == "\"a\\u000a\\\"\\\\\\u0009\"",
                    "JSON escaping incorrect");
            rejects([] { opt::Json(std::nan("")); });
        });
        test("pgbench summary preserves errors and transaction counts", [&] {
            const auto parsed = opt::parse_pgbench(
                "number of transactions actually processed: 8000/8000\n"
                "number of failed transactions: 2 (0.025%)\n"
                "latency average = 1.250 ms\n"
                "tps = 6400.125000 (without initial connection time)\n");
            require(parsed.successful == 8000 && parsed.failed == 2,
                    "pgbench counts were lost");
            require(parsed.average_latency_ms == 1.25 &&
                    parsed.transactions_per_second == 6400.125, "pgbench numbers incorrect");
            rejects([] { opt::parse_pgbench("connection refused\n"); });
            rejects([] { opt::parse_pgbench(
                "number of transactions actually processed: 10\n"
                "latency average = 1.0 ms\ntps = 100.0\n"); });
        });
        test("subprocess passes arguments literally and preserves exit code", [&] {
            const auto result = opt::run_command({argv[0], "--echo-child", "$(not-a-shell); a b"});
            require(result.exit_code == 7 && result.output == "$(not-a-shell); a b",
                    "Arguments changed or wrong exit status");
        });
        test("subprocess timeout kills and reaps child", [&] {
            rejects([&] { opt::run_command({argv[0], "--sleep-child"}, 0.05); });
        });
        test("CLI benchmark writes report and returns success or failure", [&] {
            const auto binary = (std::filesystem::path(argv[0]).parent_path() / "server-opt").string();
            auto result = opt::run_command({
                binary, "bench", server.url + "/ok", "--duration", "0.1",
                "--concurrency", "2", "--output", "results/local-bench.json",
            });
            require(result.exit_code == 0, "Successful CLI benchmark failed");
            require(opt::read_file("results/local-bench.json") == result.output,
                    "Saved report differs from stdout");
            result = opt::run_command({
                binary, "bench", server.url + "/error", "--duration", "0.05",
                "--concurrency", "1",
            });
            require(result.exit_code == 2, "CLI did not report failed requests");
        });
        std::cout << passed << " tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
