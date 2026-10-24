// HTTPS client that keeps one connection per host open for the duration of a
// refresh cycle, so several stops on the same operator share one TLS handshake.
#pragma once

#include <string>
#include <vector>

#include "esp_http_client.h"

class HttpFetcher {
public:
    ~HttpFetcher() { CloseAll(); }
    // GET when post_body is empty, otherwise POST application/json.
    // Returns true for HTTP 200 with a body.
    bool Fetch(const std::string& url, const std::string& post_body, std::string* body);
    void CloseAll();

private:
    struct Conn {
        std::string host;
        esp_http_client_handle_t client = nullptr;
    };
    struct Ctx {
        std::string* body = nullptr;
        bool overflow = false;
    };
    static esp_err_t OnEvent(esp_http_client_event_t* evt);
    bool FetchOnce(const std::string& url, const std::string& post_body, std::string* body,
                   bool fresh);
    esp_http_client_handle_t ClientFor(const std::string& url, bool fresh);

    std::vector<Conn> conns_;
    Ctx ctx_;
};
