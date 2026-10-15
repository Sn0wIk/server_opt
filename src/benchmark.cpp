#include "server_opt.hpp"

#include <algorithm>
#include <curl/curl.h>
#include <limits>
#include <memory>
#include <random>
#include <sys/resource.h>

namespace opt {
namespace {
using Clock = std::chrono::steady_clock;
using Easy = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
using Multi = std::unique_ptr<CURLM, decltype(&curl_multi_cleanup)>;
using Url = std::unique_ptr<CURLU, decltype(&curl_url_cleanup)>;
using CurlString = std::unique_ptr<char, decltype(&curl_free)>;

void check(CURLcode code) {
    if (code != CURLE_OK) throw std::runtime_error(curl_easy_strerror(code));
}
void check_multi(CURLMcode code) {
    if (code != CURLM_OK) throw std::runtime_error(curl_multi_strerror(code));
}

struct Slot {
    Easy handle{curl_easy_init(), curl_easy_cleanup};
    size_t bytes = 0;
    std::string body;
    bool check_body = false;
    bool oversize = false;
    bool allocation_failed = false;
    bool active = false;
    void reset() {
        bytes = 0;
        body.clear();
        oversize = allocation_failed = false;
    }
};

size_t receive(char* data, size_t size, size_t count, void* context) noexcept {
    auto& slot = *static_cast<Slot*>(context);
    if (size && count > std::numeric_limits<size_t>::max() / size) return 0;
    const size_t bytes = size * count;
    if (bytes > max_body - slot.bytes) {
        slot.oversize = true;
        return 0;
    }
    slot.bytes += bytes;
    if (slot.check_body) {
        try { slot.body.append(data, bytes); }
        catch (...) { slot.allocation_failed = true; return 0; }
    }
    return bytes;
}

Json counts_json(const std::map<std::string, uint64_t>& counts) {
    Json::Object result;
    for (const auto& entry : counts) result[entry.first] = entry.second;
    return result;
}

double cpu_seconds() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
    return usage.ru_utime.tv_sec + usage.ru_stime.tv_sec +
           (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
}
} // namespace

CurlRuntime::CurlRuntime() { check(curl_global_init(CURL_GLOBAL_DEFAULT)); }
CurlRuntime::~CurlRuntime() { curl_global_cleanup(); }

void validate(const BenchOptions& options) {
    if (!std::isfinite(options.duration) || options.duration <= 0 || options.duration > 3600)
        throw std::invalid_argument("duration must be > 0 and <= 3600 seconds");
    if (options.concurrency < 1 || options.concurrency > 256)
        throw std::invalid_argument("concurrency must be between 1 and 256");
    if (!std::isfinite(options.timeout) || options.timeout < 0.001 || options.timeout > 60)
        throw std::invalid_argument("timeout must be between 0.001 and 60 seconds");
    if (options.expected_status < 200 || options.expected_status > 599)
        throw std::invalid_argument("expected status must be between 200 and 599");
    if (options.expected_body && options.expected_body->size() > max_body)
        throw std::invalid_argument("expected body exceeds 1 MiB");
    if (options.url.find_first_of("\r\n\t ") != std::string::npos)
        throw std::invalid_argument("URL must not contain whitespace");
    if (options.url.rfind("http://", 0) != 0 && options.url.rfind("https://", 0) != 0)
        throw std::invalid_argument("URL must start with http:// or https://");
    Url url(curl_url(), curl_url_cleanup);
    if (!url || curl_url_set(url.get(), CURLUPART_URL, options.url.c_str(), 0) != CURLUE_OK)
        throw std::invalid_argument("Invalid URL");
    for (auto part : {CURLUPART_USER, CURLUPART_PASSWORD, CURLUPART_FRAGMENT}) {
        char* raw = nullptr;
        const auto code = curl_url_get(url.get(), part, &raw, 0);
        CurlString value(raw, curl_free);
        if (code == CURLUE_OK)
            throw std::invalid_argument("URL must not contain credentials or a fragment");
    }
}

BenchResult benchmark(const BenchOptions& options) {
    validate(options);
    // Handles remain at fixed addresses because libcurl callbacks point at slots.
    std::vector<Slot> slots(options.concurrency);
    Multi multi(curl_multi_init(), curl_multi_cleanup);
    if (!multi) throw std::runtime_error("curl_multi_init failed");
    check_multi(curl_multi_setopt(multi.get(), CURLMOPT_MAX_TOTAL_CONNECTIONS,
                                 static_cast<long>(options.concurrency)));
    check_multi(curl_multi_setopt(multi.get(), CURLMOPT_MAX_HOST_CONNECTIONS,
                                 static_cast<long>(options.concurrency)));
    check_multi(curl_multi_setopt(multi.get(), CURLMOPT_MAXCONNECTS,
                                 static_cast<long>(options.concurrency)));
    check_multi(curl_multi_setopt(multi.get(), CURLMOPT_PIPELINING, 0L));
    for (auto& slot : slots) {
        if (!slot.handle) throw std::runtime_error("curl_easy_init failed");
        slot.check_body = options.expected_body.has_value();
        CURL* easy = slot.handle.get();
        check(curl_easy_setopt(easy, CURLOPT_URL, options.url.c_str()));
        check(curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "http,https"));
        check(curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1));
        check(curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L));
        check(curl_easy_setopt(easy, CURLOPT_PROXY, ""));
        check(curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L));
        check(curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS,
                              static_cast<long>(std::ceil(options.timeout * 1000))));
        check(curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS,
                              static_cast<long>(std::ceil(options.timeout * 1000))));
        check(curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "identity"));
        check(curl_easy_setopt(easy, CURLOPT_HTTP_CONTENT_DECODING, 0L));
        check(curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, receive));
        check(curl_easy_setopt(easy, CURLOPT_WRITEDATA, &slot));
        check(curl_easy_setopt(easy, CURLOPT_PRIVATE, &slot));
    }

    BenchResult result;
    std::map<std::string, uint64_t> statuses, errors;
    std::vector<double> samples;
    samples.reserve(max_samples);
    std::mt19937_64 rng(0);
    uint64_t body_bytes = 0;
    double latency_sum = 0;
    const double cpu_start = cpu_seconds();
    const auto started = Clock::now();
    const auto deadline = started + std::chrono::duration<double>(options.duration);
    size_t active = 0;
    auto submit = [&](Slot& slot) {
        slot.reset();
        check_multi(curl_multi_add_handle(multi.get(), slot.handle.get()));
        slot.active = true;
        ++active;
        ++result.attempts;
    };
    for (auto& slot : slots) submit(slot);

    while (active && !interrupted) {
        int running = 0;
        check_multi(curl_multi_perform(multi.get(), &running));
        int remaining = 0;
        while (CURLMsg* message = curl_multi_info_read(multi.get(), &remaining)) {
            if (message->msg != CURLMSG_DONE) continue;
            Slot* slot = nullptr;
            check(curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &slot));
            long status = 0;
            double seconds = 0;
            check(curl_easy_getinfo(message->easy_handle, CURLINFO_RESPONSE_CODE, &status));
            check(curl_easy_getinfo(message->easy_handle, CURLINFO_TOTAL_TIME, &seconds));
            if (status) ++statuses[std::to_string(status)];
            std::string error;
            if (slot->oversize) error = "body_exceeds_1_mib";
            else if (slot->allocation_failed) error = "body_allocation_failed";
            else if (message->data.result != CURLE_OK)
                error = curl_easy_strerror(message->data.result);
            else if (status != options.expected_status) error = "unexpected_http_status";
            else if (options.expected_body && slot->body != *options.expected_body)
                error = "body_mismatch";
            if (error.empty()) {
                ++result.successful;
                body_bytes += slot->bytes;
                const double latency = seconds * 1000;
                latency_sum += latency;
                if (samples.size() < max_samples) samples.push_back(latency);
                else {
                    std::uniform_int_distribution<uint64_t> pick(0, result.successful - 1);
                    const auto index = pick(rng);
                    if (index < max_samples) samples[index] = latency;
                }
            } else ++errors[error];
            check_multi(curl_multi_remove_handle(multi.get(), slot->handle.get()));
            slot->active = false;
            --active;
            if (Clock::now() < deadline && !interrupted) submit(*slot);
        }
        if (active && !interrupted)
            check_multi(curl_multi_poll(multi.get(), nullptr, 0, 50, nullptr));
    }
    for (auto& slot : slots) {
        if (slot.active) {
            check_multi(curl_multi_remove_handle(multi.get(), slot.handle.get()));
            ++errors["interrupted"];
        }
    }
    const double elapsed = std::chrono::duration<double>(Clock::now() - started).count();
    const double client_cpu = cpu_seconds() - cpu_start;
    result.failed = result.attempts - result.successful;
    result.requests_per_second = result.successful / elapsed;
    std::sort(samples.begin(), samples.end());
    auto percentile = [&](double fraction) -> Json {
        if (samples.empty()) return nullptr;
        return samples[static_cast<size_t>(std::ceil(samples.size() * fraction)) - 1];
    };
    if (!samples.empty())
        result.p95 = samples[static_cast<size_t>(std::ceil(samples.size() * 0.95)) - 1];
    result.report = Json::Object{
        {"schema_version", 2},
        {"created_at", timestamp()},
        {"engine", "C++17 / libcurl multi / HTTP/1.1"},
        {"libcurl", curl_version()},
        {"url", options.url},
        {"concurrency", options.concurrency},
        {"requested_duration_seconds", options.duration},
        {"elapsed_seconds_including_inflight_requests", elapsed},
        {"timeout_seconds", options.timeout},
        {"attempts", result.attempts},
        {"successful", result.successful},
        {"failed", result.failed},
        {"successful_requests_per_second", result.requests_per_second},
        {"response_body_bytes", body_bytes},
        {"client_process_cpu_seconds", client_cpu},
        {"http_status_counts", counts_json(statuses)},
        {"errors", counts_json(errors)},
        {"expected_status", options.expected_status},
        {"body_validation", options.expected_body ? "exact bytes" : "disabled"},
        {"successful_latency_ms", Json::Object{
            {"mean", result.successful ? Json(latency_sum / result.successful) : Json()},
            {"p50", percentile(0.5)}, {"p95", percentile(0.95)}, {"p99", percentile(0.99)},
            {"sample_count", samples.size()},
            {"percentiles_sampled", samples.size() < result.successful},
        }},
        {"limitations", Json::Array{
            "Closed-loop load, not a fixed arrival rate or SLA test.",
            "Client, networking and shared host resources can limit throughput.",
            "Latency includes connection/TLS on new connections; failures are separate.",
            "Direct connections; proxy environment variables are ignored. No redirects.",
            "Response bodies above 1 MiB are rejected. No energy measurement.",
        }},
    };
    return result;
}

} // namespace opt
