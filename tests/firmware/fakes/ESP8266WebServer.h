#pragma once
#include <map>
#include <vector>

#include "Arduino.h"

enum HTTPMethod { HTTP_ANY, HTTP_GET, HTTP_POST };

struct FakeResponse {
  int code = 0;
  std::string type;
  std::string body;
};

class ESP8266WebServer {
 public:
  explicit ESP8266WebServer(int) {}
  void on(const char* uri, HTTPMethod method, std::function<void()> fn) {
    routes_.push_back({uri, method, fn});
  }
  void onNotFound(std::function<void()> fn) { not_found_ = fn; }
  void begin() { started = true; }
  void handleClient() {}

  void send(int code, const char* type, const String& body) { reply_ = {code, type, body.str()}; }
  void send(int code, const char* type, const char* body) { reply_ = {code, type, body}; }
  void send_P(int code, PGM_P type, PGM_P body) { reply_ = {code, type, body}; }
  void sendHeader(const char*, const char*) {}
  bool hasArg(const char* name) { return args_.count(name) > 0; }
  String arg(const char* name) { return hasArg(name) ? String(args_[name]) : String(""); }

  // Test helper: dispatch a request the way ESP8266WebServer does
  // (a route only matches its own method; anything else is "not found").
  FakeResponse request(HTTPMethod method, const std::string& path,
                       std::map<std::string, std::string> args = {}) {
    args_ = std::move(args);
    reply_ = {};
    for (auto& r : routes_) {
      if (r.uri == path && (r.method == HTTP_ANY || r.method == method)) {
        r.fn();
        return reply_;
      }
    }
    if (not_found_) not_found_();
    return reply_;
  }

  bool started = false;

 private:
  struct Route {
    std::string uri;
    HTTPMethod method;
    std::function<void()> fn;
  };
  std::vector<Route> routes_;
  std::function<void()> not_found_;
  std::map<std::string, std::string> args_;
  FakeResponse reply_;
};
