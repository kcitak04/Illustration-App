#include "LocalServer.h"

namespace dat
{
namespace
{
juce::String urlDecode (const juce::String& s) { return juce::URL::removeEscapeChars (s.replace ("+", " ")); }

// buffered reader on a socket
struct Reader
{
    juce::StreamingSocket& s;
    const std::atomic<bool>& stopping;
    juce::MemoryBlock buf; size_t pos = 0, len = 0;
    Reader (juce::StreamingSocket& sock, const std::atomic<bool>& stop) : s (sock), stopping (stop) { buf.setSize (65536); }
    bool fill (int timeoutMs)
    {
        if (pos == len) { pos = len = 0; }
        if (len == buf.getSize()) { if (pos == 0) return false; std::memmove (buf.getData(), (char*) buf.getData() + pos, len - pos); len -= pos; pos = 0; }
        // wait in short slices: closing the socket from another thread does not wake a waiting poll() everywhere
        // (macOS), and the server's destructor must not hang for the whole keep-alive timeout
        for (int waited = 0;; waited += 200)
        {
            if (stopping.load() || waited >= timeoutMs) return false;
            const int r = s.waitUntilReady (true, std::min (200, timeoutMs - waited));
            if (r < 0) return false;
            if (r == 1) break;
        }
        int n = s.read ((char*) buf.getData() + len, (int) (buf.getSize() - len), false);
        if (n <= 0) return false;
        len += (size_t) n; return true;
    }
    bool line (juce::String& out, int timeoutMs)
    {
        std::string acc;
        for (;;)
        {
            while (pos < len)
            {
                char c = ((char*) buf.getData())[pos++];
                if (c == '\n') { if (! acc.empty() && acc.back() == '\r') acc.pop_back(); out = juce::String (acc); return true; }
                acc.push_back (c);
                if (acc.size() > 16384) return false;
            }
            if (! fill (timeoutMs)) return false;
        }
    }
    bool bytes (juce::MemoryBlock& out, size_t n, int timeoutMs)
    {
        out.setSize (n);
        size_t got = 0;
        while (got < n)
        {
            if (pos == len && ! fill (timeoutMs)) return false;
            size_t k = std::min (n - got, len - pos);
            std::memcpy ((char*) out.getData() + got, (char*) buf.getData() + pos, k);
            pos += k; got += k;
        }
        return true;
    }
};

bool writeAll (juce::StreamingSocket& s, const void* d, size_t n)
{
    size_t done = 0;
    while (done < n)
    {
        int w = s.write ((const char*) d + done, (int) std::min<size_t> (n - done, 1 << 20));
        if (w <= 0) return false;
        done += (size_t) w;
    }
    return true;
}

const char* reason (int status)
{
    switch (status) { case 200: return "OK"; case 204: return "No Content"; case 400: return "Bad Request"; case 403: return "Forbidden"; case 404: return "Not Found"; default: return "Error"; }
}
}

LocalServer::LocalServer()
{
    // a fixed port keeps the page's origin (and its local storage) stable between sessions
    for (int p = 47811; p < 47851; ++p)
    {
        // JUCE listeners use SO_REUSEADDR, and on Windows that lets a second process bind a port that is already
        // listening (connections would then be split between the two). So first check nobody answers on it -
        // e.g. Ableton's plugin scanner or another DAW with its own DATTRACK.
        {
            juce::StreamingSocket probe;
            if (probe.connect ("127.0.0.1", p, 150)) continue;
        }
        auto s = std::make_unique<juce::StreamingSocket>();
        if (s->createListener (p, "127.0.0.1")) { listener = std::move (s); port = p; break; }
    }
    if (listener) acceptThread = std::thread ([this] { acceptLoop(); });
}

LocalServer::~LocalServer()
{
    stopping = true;
    if (listener) listener->close();
    if (acceptThread.joinable()) acceptThread.join();
    std::vector<Worker> ws;
    {
        std::lock_guard<std::mutex> l (lock);
        for (auto* s : open) s->close();
        ws.swap (workers);
    }
    for (auto& w : ws) if (w.thread.joinable()) w.thread.join();
}

int LocalServer::registerInstance (const juce::String& token, Handler h)
{
    std::lock_guard<std::mutex> l (lock);
    int id = nextId++;
    instances[id] = { token, std::make_shared<Handler> (std::move (h)) };
    return id;
}

void LocalServer::unregisterInstance (int id)
{
    std::shared_ptr<Handler> keep;
    {
        std::lock_guard<std::mutex> l (lock);
        auto it = instances.find (id);
        if (it == instances.end()) return;
        keep = it->second.handler;
        instances.erase (it);
    }
    // wait until no request is still running inside this instance's handler
    while (keep.use_count() > 1) juce::Thread::sleep (2);
}

void LocalServer::acceptLoop()
{
    while (! stopping.load())
    {
        std::unique_ptr<juce::StreamingSocket> c (listener->waitForNextConnection());
        if (! c) { if (stopping.load()) break; juce::Thread::sleep (20); continue; }
        std::lock_guard<std::mutex> l (lock);
        // join connections that have finished (a long session opens many)
        for (auto it = workers.begin(); it != workers.end();)
            if (it->done->load()) { it->thread.join(); it = workers.erase (it); } else ++it;
        open.push_back (c.get());
        auto done = std::make_shared<std::atomic<bool>> (false);
        workers.push_back ({ std::thread ([this, done, s = c.release()] { serve (std::unique_ptr<juce::StreamingSocket> (s)); *done = true; }), done });
    }
}

void LocalServer::serve (std::unique_ptr<juce::StreamingSocket> sock)
{
    Reader rd (*sock, stopping);
    const juce::String hostOk1 = "127.0.0.1:" + juce::String (port), hostOk2 = "localhost:" + juce::String (port);
    while (! stopping.load())
    {
        juce::String first;
        if (! rd.line (first, 30000)) break;
        if (first.isEmpty()) continue;
        HttpRequest req;
        auto parts = juce::StringArray::fromTokens (first, " ", "");
        if (parts.size() < 2) break;
        req.method = parts[0];
        auto target = parts[1];
        req.path = target.upToFirstOccurrenceOf ("?", false, false);
        for (auto& kv : juce::StringArray::fromTokens (target.fromFirstOccurrenceOf ("?", false, false), "&", ""))
            if (kv.isNotEmpty()) req.query[urlDecode (kv.upToFirstOccurrenceOf ("=", false, false))] = urlDecode (kv.fromFirstOccurrenceOf ("=", false, false));
        juce::String h; bool ok = true;
        while ((ok = rd.line (h, 10000)) && h.isNotEmpty())
            req.headers.set (h.upToFirstOccurrenceOf (":", false, false).trim().toLowerCase(), h.fromFirstOccurrenceOf (":", false, false).trim());
        if (! ok) break;
        auto clen = req.headers["content-length"].getLargeIntValue();
        if (clen < 0 || clen > (juce::int64) 512 * 1024 * 1024) break;
        if (clen > 0 && ! rd.bytes (req.body, (size_t) clen, 10000)) break;

        HttpResponse res;
        auto host = req.headers["host"];
        if (host != hostOk1 && host != hostOk2) res = HttpResponse::text ("{\"error\":\"host\"}", "application/json", 403);   // DNS rebinding guard
        else res = dispatch (req);

        juce::String head = "HTTP/1.1 " + juce::String (res.status) + " " + reason (res.status) + "\r\n"
                          + "Content-Type: " + res.contentType + "\r\n"
                          + "Content-Length: " + juce::String ((juce::int64) res.body.getSize()) + "\r\n"
                          + "Cache-Control: no-store\r\nConnection: keep-alive\r\n\r\n";
        if (! writeAll (*sock, head.toRawUTF8(), head.getNumBytesAsUTF8())) break;
        if (res.body.getSize() && ! writeAll (*sock, res.body.getData(), res.body.getSize())) break;
        if (req.headers["connection"].equalsIgnoreCase ("close")) break;
    }
    std::lock_guard<std::mutex> l (lock);
    open.erase (std::remove (open.begin(), open.end(), sock.get()), open.end());
    sock->close();
}

HttpResponse LocalServer::dispatch (const HttpRequest& req)
{
    // "/<route>/<id>" with a matching token goes to that instance, everything else is a static file
    auto segs = juce::StringArray::fromTokens (req.path, "/", "");
    segs.removeEmptyStrings();
    if (segs.size() == 2 && segs[1].containsOnly ("0123456789"))
    {
        std::shared_ptr<Handler> h;
        {
            std::lock_guard<std::mutex> l (lock);
            auto it = instances.find (segs[1].getIntValue());
            if (it == instances.end()) return HttpResponse::text ("{\"error\":\"gone\"}", "application/json", 404);
            if (it->second.token != req.q ("t")) return HttpResponse::text ("{\"error\":\"token\"}", "application/json", 403);
            h = it->second.handler;
        }
        return (*h) (req);
    }
    Handler s;
    { std::lock_guard<std::mutex> l (lock); s = staticHandler; }
    return s ? s (req) : HttpResponse::empty (404);
}
}
