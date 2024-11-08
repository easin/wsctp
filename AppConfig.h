#pragma once
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <string>
#include <yaml-cpp/yaml.h>

class AppConfig {
public:
    static std::string FrontAddr;
    static std::string FrontMdAddr;
    static std::string BrokerID;
    static std::string UserID;
    static std::string Password;
    static std::string InvestorID;
    static std::string UserProductInfo;
    static std::string AuthCode;
    static std::string AppID;
    static std::string ExchangeID;
    static std::string InstrumentID;
    static std::string CurrencyID;
    static int bIsUsingUdp;
    static int bIsMulticast;
    static std::string REDIS_HOST;
    static int REDIS_DB;
    static int REDIS_PORT;
    static int REDIS_POOL_SIZE;
    static int REDIS_RECONNECT_TIMES; //重连次数
    static int REDIS_INTERVAL_SECONDES;//cpp_redis::client重连间隔秒数
    static int WEBSOCKET_PORT;
    static int HEART_INTERVAL; //socket心跳，简化不需要设置
    static int LOG_LEVEL;
    static std::string CZCE;
    // 定义交易所代码的静态常量数组
    static const std::vector<std::string> EXCHANGE_IDS;

    static void fromYaml(const std::string& filename);
};

#endif // APP_CONFIG_H