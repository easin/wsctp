#include "stdafx.h"
#include "RedisUtil.h"

std::vector<std::unique_ptr<cpp_redis::client>> RedisUtil::availableClients;
std::mutex RedisUtil::clientsMutex;
std::mutex RedisUtil::initedMutex;
bool RedisUtil::initialized = false;

void RedisUtil::initClients() {
    std::lock_guard<std::mutex> lock(initedMutex);
    if (!initialized) {
        for (int i = 0; i < AppConfig::REDIS_POOL_SIZE; ++i) {
            auto client = std::make_unique<cpp_redis::client>();
            //lambda表达式闭包：
            client->connect(AppConfig::REDIS_HOST, AppConfig::REDIS_PORT, [](const std::string& host, std::size_t port, cpp_redis::client::connect_state status) {
                if (status == cpp_redis::client::connect_state::ok) {
                    spdlog::info("连接到 Redis 服务器 {}:{} 成功", host, port);
                }
                else {
                    spdlog::error("连接到 Redis 服务器 {}:{} 失败", host, port);
                }
                });
            client->auth("", [](const cpp_redis::reply& reply) {
                if (reply.is_error()) {
                    spdlog::error("Redis 认证失败: {}", reply.as_string());
                }
                else {
                    spdlog::debug("Redis 认证成功");
                }
                });
            client->select(AppConfig::REDIS_DB, [](const cpp_redis::reply& reply) {
                if (reply.is_error()) {
                    spdlog::error("选择 Redis 数据库失败: {}", reply.as_string());
                }
                else {
                    spdlog::debug("选择 Redis 数据库{}成功", AppConfig::REDIS_DB);
                }
                });
            availableClients.push_back(std::move(client));
        }
        initialized = true;
        //spdlog::debug("Redis连接池初始化成功，共{}个连接", AppConfig::REDIS_POOL_SIZE);
    }
}

std::optional<json> RedisUtil::pullMsg(const std::string& queue) {
    std::unique_ptr<cpp_redis::client> clientPtr = getClient();
    std::string msg;
    clientPtr->rpop(queue, [&msg](const cpp_redis::reply& reply) {
        if (reply.is_error()) {
            spdlog::error("获取消息错误: {}", reply.as_string());
            msg = "";
        }
        else {
            msg = reply.as_array()[0].as_string();
        }
        });
    clientPtr->sync_commit();
    return msg.empty() ? std::nullopt : std::make_optional(json::parse(msg));
    RedisUtil::returnClient(std::move(clientPtr));
}

void RedisUtil::pushMsg(const std::string& queue, const std::string& jsonStrItem) {
    std::unique_ptr<cpp_redis::client> clientPtr = getClient();
    clientPtr->lpushx(queue, jsonStrItem, [](const cpp_redis::reply& reply) {
        if (reply.is_error()) {
            spdlog::error("推送消息错误: {}", reply.as_string());
        }
        });
    clientPtr->sync_commit();
    RedisUtil::returnClient(std::move(clientPtr));
}
std::vector<std::string> RedisUtil::listMsg(const std::string& queue) {
    std::unique_ptr<cpp_redis::client> clientPtr = getClient();
    std::vector<std::string> items;
    std::promise<void> promise;
    std::future<void> future = promise.get_future();

    clientPtr->lrange(queue, 0, -1, [&items, &promise](const cpp_redis::reply& reply) {
        if (reply.is_error()) {
            spdlog::error("获取列表错误: {}", reply.as_string());
        }
        else {
            // 假设返回的是字符串列表
            const std::vector<cpp_redis::reply>& replies = reply.as_array();
            for (const auto& item : replies) {
                items.push_back(item.as_string());
            }
        }
        promise.set_value();
        });

    clientPtr->sync_commit();
    future.wait(); // 等待异步回调完成
    returnClient(std::move(clientPtr));
    return items;
}

void RedisUtil::clearMsg(const std::string& queue) {
    std::unique_ptr<cpp_redis::client> clientPtr = getClient();
    clientPtr->del({ queue }, [](const cpp_redis::reply& reply) {
        if (reply.is_error()) {
            spdlog::error("清空列表错误: {}", reply.as_string());
        }
        });
    clientPtr->sync_commit();
    returnClient(std::move(clientPtr));
}

std::optional<std::string> RedisUtil::getStr(const std::string& key) {
    std::unique_ptr<cpp_redis::client> clientPtr = getClient();
    std::string value;
    clientPtr->get(key, [&value](const cpp_redis::reply& reply) {
        if (reply.is_error()) {
            spdlog::error("获取字符串错误: {}", reply.as_string());
            value = "";
        }
        else {
            value = reply.as_string();
        }
        });
    clientPtr->sync_commit();
    RedisUtil::returnClient(std::move(clientPtr));
    return value.empty() ? std::nullopt : std::make_optional(value);
}

void RedisUtil::setStr(const std::string& key, const std::string& value) {
    std::unique_ptr<cpp_redis::client> clientPtr = getClient();
    clientPtr->set(key, value, [](const cpp_redis::reply& reply) {
        if (reply.is_error()) {
            spdlog::error("设置字符串错误: {}", reply.as_string());
        }
        });
    clientPtr->sync_commit();
    RedisUtil::returnClient(std::move(clientPtr));
}


// 从连接池中获取一个 client 对象
std::unique_ptr<cpp_redis::client> RedisUtil::getClient() {
    std::lock_guard<std::mutex> lock(clientsMutex);
    if (!availableClients.empty()) {
        std::unique_ptr<cpp_redis::client> client = std::move(availableClients.back());
        availableClients.pop_back();
        //spdlog::debug("借走了，还有{}个可用", RedisUtil::availableClients.size());
        return client;
    }
    throw std::runtime_error("无连接可用.");
}

// 归还 client 对象到连接池
void RedisUtil::returnClient(std::unique_ptr<cpp_redis::client> client) {
    std::lock_guard<std::mutex> lock(clientsMutex);
    if (client) {
        availableClients.push_back(std::move(client));
    }
    //spdlog::debug("增加了，还有{}个可用", RedisUtil::availableClients.size());
}
