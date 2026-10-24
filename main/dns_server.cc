#include "dns_server.h"

#include <atomic>
#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

namespace dns_server {
namespace {

constexpr char kTag[] = "dns";
std::atomic<bool> g_running{false};
std::atomic<int> g_sock{-1};
TaskHandle_t g_task = nullptr;

void Task(void*) {
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(kTag, "socket failed");
        g_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ESP_LOGE(kTag, "bind failed");
        close(sock);
        g_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    timeval tv = {1, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    g_sock = sock;

    uint8_t buf[512];
    while (g_running) {
        sockaddr_in from = {};
        socklen_t from_len = sizeof(from);
        const int n = recvfrom(sock, buf, sizeof(buf) - 16, 0,
                               reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n < 12) continue;
        // Only standard queries with exactly one question.
        if ((buf[2] & 0x80) != 0 || buf[4] != 0 || buf[5] != 1) continue;
        // Find the end of the question (QNAME + QTYPE + QCLASS).
        int p = 12;
        while (p < n && buf[p] != 0) p += buf[p] + 1;
        p += 5;
        if (p > n) continue;
        const uint16_t qtype = static_cast<uint16_t>((buf[p - 4] << 8) | buf[p - 3]);

        buf[2] = 0x84 | (buf[2] & 0x01);  // response, authoritative, keep RD
        buf[3] = 0x80;                     // RA, no error
        buf[6] = 0;
        buf[7] = qtype == 1 ? 1 : 0;       // one answer only for A queries
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        int len = p;
        if (qtype == 1) {
            const uint8_t answer[] = {0xC0, 0x0C,             // name: pointer to question
                                      0x00, 0x01, 0x00, 0x01, // A, IN
                                      0x00, 0x00, 0x00, 0x3C, // TTL 60
                                      0x00, 0x04, 192, 168, 4, 1};
            std::memcpy(buf + len, answer, sizeof(answer));
            len += sizeof(answer);
        }
        sendto(sock, buf, len, 0, reinterpret_cast<sockaddr*>(&from), from_len);
    }
    close(sock);
    g_sock = -1;
    g_task = nullptr;
    vTaskDelete(nullptr);
}

}  // namespace

void Start() {
    if (g_running) return;
    g_running = true;
    xTaskCreate(Task, "dns", 3072, nullptr, 4, &g_task);
}

void Stop() { g_running = false; }

}  // namespace dns_server
