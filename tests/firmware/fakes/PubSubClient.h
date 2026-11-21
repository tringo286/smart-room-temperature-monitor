// Fake MQTT client. The "broker" is just a flag: when it is unreachable,
// connect() costs fake::broker_timeout_ms of blocked time (like a TCP connect
// timeout on the real board) and fails.
#pragma once
#include <vector>

#include "ESP8266WiFi.h"

namespace fake {
inline bool broker_up = true;
inline unsigned long broker_timeout_ms = 5000;
inline unsigned long blocked_ms = 0;  // total time spent inside connect()
inline int connect_attempts = 0;
}  // namespace fake

struct FakePublish {
  std::string topic;
  std::string payload;
  bool retained;
};

class PubSubClient {
 public:
  explicit PubSubClient(WiFiClient&) {}

  void setServer(const char* host, uint16_t port) { host_ = host; port_ = port; }
  void setCallback(std::function<void(char*, uint8_t*, unsigned int)> cb) { cb_ = cb; }
  bool setBufferSize(uint16_t n) { buffer_size = n; return true; }
  void setSocketTimeout(uint16_t) {}

  bool connect(const char* id, const char* will_topic, uint8_t, bool will_retain, const char* will_msg) {
    fake::connect_attempts++;
    if (!fake::broker_up) {
      // Whether WiFiClient::setTimeout() shortens a TCP connect depends on
      // the ESP8266 core version, so assume the worst case.
      unsigned long cost = fake::broker_timeout_ms;
      fake::now_ms += cost;
      fake::blocked_ms += cost;
      connected_ = false;
      return false;
    }
    client_id = id;
    will = {will_topic, will_msg, will_retain};
    connected_ = true;
    return true;
  }
  bool connected() {
    if (!fake::broker_up) connected_ = false;
    return connected_;
  }
  bool loop() { return connected(); }
  int state() { return connected_ ? 0 : -2; }

  bool publish(const char* topic, const char* payload, bool retained = false) {
    if (!connected()) return false;
    // PubSubClient drops messages that do not fit in its buffer.
    if (strlen(topic) + strlen(payload) + 7 > buffer_size) return false;
    published.push_back({topic, payload, retained});
    return true;
  }
  bool subscribe(const char* topic) {
    if (!connected()) return false;
    subscriptions.push_back(topic);
    return true;
  }

  // Test helper: the broker delivers a message to the device.
  void deliver(const std::string& topic, const std::string& payload) {
    std::vector<char> t(topic.begin(), topic.end());
    t.push_back('\0');
    std::vector<uint8_t> p(payload.begin(), payload.end());
    cb_(t.data(), p.data(), (unsigned int)p.size());
  }

  std::vector<FakePublish> published;
  std::vector<std::string> subscriptions;
  FakePublish will;
  std::string client_id;
  uint16_t buffer_size = 256;

 private:
  std::function<void(char*, uint8_t*, unsigned int)> cb_;
  std::string host_;
  uint16_t port_ = 0;
  bool connected_ = false;
};
