// Minimal captive-portal DNS: answers every A query with 192.168.4.1 so
// phones joining the set-up network open the settings page automatically.
#pragma once

namespace dns_server {
void Start();
void Stop();
}  // namespace dns_server
