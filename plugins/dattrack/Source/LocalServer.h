// Minimal HTTP/1.1 server on 127.0.0.1 shared by every DATTRACK plugin instance in the process.
// The embedded DATTRACK page is served from here (localhost is a secure context, so AudioWorklet works in
// every web view), and the page streams its audio / state / exported files back over keep-alive POSTs.
#pragma once
#include <juce_core/juce_core.h>
#include <map>
#include <mutex>
#include <thread>

namespace dat
{
struct HttpRequest
{
    juce::String method, path;
    std::map<juce::String, juce::String> query;
    juce::StringPairArray headers;
    juce::MemoryBlock body;
    juce::String q (const juce::String& k, const juce::String& def = {}) const { auto it = query.find (k); return it == query.end() ? def : it->second; }
};

struct HttpResponse
{
    int status = 200;
    juce::String contentType = "application/json";
    juce::MemoryBlock body;
    static HttpResponse text (const juce::String& s, const juce::String& type = "application/json", int status = 200)
    {
        HttpResponse r; r.status = status; r.contentType = type; r.body.append (s.toRawUTF8(), s.getNumBytesAsUTF8()); return r;
    }
    static HttpResponse data (const void* d, size_t n, const juce::String& type) { HttpResponse r; r.contentType = type; r.body.append (d, n); return r; }
    static HttpResponse empty (int status = 204) { HttpResponse r; r.status = status; return r; }
};

class LocalServer
{
public:
    using Handler = std::function<HttpResponse (const HttpRequest&)>;

    LocalServer();
    ~LocalServer();

    int getPort() const { return port; }
    bool isRunning() const { return listener != nullptr; }

    // static files ("/", "/dt-bridge.js", ...)
    void setStaticHandler (Handler h) { std::lock_guard<std::mutex> l (lock); staticHandler = std::move (h); }
    // per-instance routes "/<route>/<id>?t=<token>"
    int registerInstance (const juce::String& token, Handler h);
    void unregisterInstance (int id);

private:
    void acceptLoop();
    void serve (std::unique_ptr<juce::StreamingSocket> s);
    HttpResponse dispatch (const HttpRequest&);

    std::unique_ptr<juce::StreamingSocket> listener;
    int port = 0;
    std::thread acceptThread;
    std::atomic<bool> stopping { false };

    std::mutex lock;   // guards everything below
    Handler staticHandler;
    struct Inst { juce::String token; std::shared_ptr<Handler> handler; };
    std::map<int, Inst> instances;
    int nextId = 1;
    struct Worker { std::thread thread; std::shared_ptr<std::atomic<bool>> done; };
    std::vector<Worker> workers;
    std::vector<juce::StreamingSocket*> open;
};
}
