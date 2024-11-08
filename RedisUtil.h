#ifndef REDISUTIL_H
#define REDISUTIL_H

#include <cpp_redis/cpp_redis.h>
#include <string>
#include <optional>
#include <mutex>
#include <vector>
#include <memory>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include "AppConfig.h"

using json = nlohmann::json;

class RedisUtil {
public:
    RedisUtil() = delete; // 禁用构造函数
    ~RedisUtil() = delete; // 禁用析构函数

    static std::optional<json> pullMsg(const std::string& queue);
    static void pushMsg(const std::string& queue, const std::string& jsonStrItem);
    static std::vector<std::string> listMsg(const std::string& queue);//获取所有不删除
    static void clearMsg(const std::string& queue);//获取所有不删除
    static std::optional<std::string> getStr(const std::string& key);
    static void setStr(const std::string& key, const std::string& value);
    static void setInt(const std::string& key, int value);
    static std::optional<int> getInt(const std::string& key);
    static void setFloat(const std::string& key, float value);
    static std::optional<float> getFloat(const std::string& key);
    static void setDouble(const std::string& key, double value);
    static std::optional<double> getDouble(const std::string& key);
    // 初始化连接池
    static void initClients();
    // 从连接池中获取一个 client 对象
    static std::unique_ptr<cpp_redis::client> getClient();

    // 归还 client 对象到连接池
    static void returnClient(std::unique_ptr<cpp_redis::client> client);

private:
    

    // 连接池
    static std::vector<std::unique_ptr<cpp_redis::client>> availableClients;
    // 互斥锁，用于同步访问连接池
    static std::mutex clientsMutex;
    // 初始化互斥锁
    static std::mutex initedMutex;
    // 标记是否已经初始化
    static bool initialized;
    
};

#endif // REDISUTIL_H