#pragma once
#include <functional>
#include <memory>
#include <string>

// Forward declarations
class TcpConnection;
class Buffer;

using TcpConnectionPtr = std::shared_ptr<TcpConnection>;

// Callbacks for connection lifecycle
using ConnectionCallback = std::function<void(const TcpConnectionPtr&)>;
using MessageCallback    = std::function<void(const TcpConnectionPtr&, Buffer*)>;
using WriteCompleteCallback = std::function<void(const TcpConnectionPtr&)>;
using CloseCallback      = std::function<void(const TcpConnectionPtr&)>;

// Callback for HTTP-style request handling
using HttpRequestCallback = std::function<void(const TcpConnectionPtr&, const std::string&)>;
