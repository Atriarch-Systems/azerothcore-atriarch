#ifndef MOD_OLLAMA_CHAT_QUERYMANAGER_H
#define MOD_OLLAMA_CHAT_QUERYMANAGER_H

#include <cstdint>
#include <string>
#include <future>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>

// Optional per-request overrides for QueryOllamaAPI. Any field left empty falls back to
// the module-wide OllamaChat.* config value, so a default-constructed instance reproduces
// the classic single-argument behavior exactly.
struct OllamaQueryOptions
{
    std::optional<uint32_t> numPredict;
    std::optional<std::string> systemPrompt;
    std::optional<bool> think;
};

std::string QueryOllamaAPI(const std::string& prompt);
std::string QueryOllamaAPI(const std::string& prompt, OllamaQueryOptions const& opts);

class QueryManager {
public:
    QueryManager();
    void setMaxConcurrentQueries(int maxQueries);
    std::future<std::string> submitQuery(const std::string& prompt, OllamaQueryOptions opts = {});

private:
    struct QueryTask {
        std::string prompt;
        OllamaQueryOptions opts;
        std::promise<std::string> promise;
    };

    void processQuery(const std::string& prompt, OllamaQueryOptions opts, std::promise<std::string> promise);

    int maxConcurrentQueries; // 0 means no limit
    int currentQueries;
    std::mutex mutex_;
    std::queue<QueryTask> taskQueue;
};

#endif // MOD_OLLAMA_CHAT_QUERYMANAGER_H
