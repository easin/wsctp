#include "stdafx.h"
#include "AppConfig.h"
#include <iostream>

std::string AppConfig::FrontAddr;
std::string AppConfig::FrontMdAddr;
std::string AppConfig::BrokerID;
std::string AppConfig::UserID;
std::string AppConfig::Password;
std::string AppConfig::InvestorID;
std::string AppConfig::UserProductInfo;
std::string AppConfig::AuthCode;
std::string AppConfig::AppID;
std::string AppConfig::InstrumentID;
std::string AppConfig::ExchangeID;
std::string AppConfig::CurrencyID="CNY";
int AppConfig::bIsUsingUdp;
int AppConfig::bIsMulticast;
std::string AppConfig::REDIS_HOST;
int AppConfig::REDIS_DB;
int AppConfig::REDIS_PORT;
int AppConfig::REDIS_POOL_SIZE;
int AppConfig::REDIS_RECONNECT_TIMES;
int AppConfig::REDIS_INTERVAL_SECONDES;
int AppConfig::WEBSOCKET_PORT;
int AppConfig::HEART_INTERVAL;
int AppConfig::LOG_LEVEL;
std::string AppConfig::CZCE;
// 初始化静态常量数组
const std::vector<std::string> AppConfig::EXCHANGE_IDS= {
    "SHFE",  // 上海期货交易所
    "DCE",   // 大连商品交易所
    "CZCE",  // 郑州商品交易所
    "CFFEX", // 中国金融交易所
    "INE",   // 上海能源中心（原油在这里）
    "GFEX"   // 广州期货交易所
};

void AppConfig::fromYaml(const std::string& filename) {
    YAML::Node configNode = YAML::LoadFile(filename);
    if (configNode) {
        if (configNode["profile"] && !configNode["profile"].IsNull() && !configNode["profile"].as<std::string>().empty()) {
            // 使用profile节点的值来构造新的配置文件名
            std::string profile = configNode["profile"].as<std::string>() + ".yaml";
            // 重新加载配置文件
            configNode = YAML::LoadFile(profile);
        }
        //解析account
        const YAML::Node& cfgAccount = configNode["account"];
        if (cfgAccount) {
            FrontAddr = cfgAccount["FrontAddr"].as<std::string>();
            FrontMdAddr = cfgAccount["FrontMdAddr"].as<std::string>();
            BrokerID = cfgAccount["BrokerID"].as<std::string>();
            UserID = cfgAccount["UserID"].as<std::string>();
            Password = cfgAccount["Password"].as<std::string>();
            InvestorID = cfgAccount["InvestorID"].as<std::string>();
            UserProductInfo = cfgAccount["UserProductInfo"].as<std::string>();
            AuthCode = cfgAccount["AuthCode"].as<std::string>();
            AppID = cfgAccount["AppID"].as<std::string>();
            InstrumentID = cfgAccount["InstrumentID"].as<std::string>();
            ExchangeID = cfgAccount["ExchangeID"].as<std::string>();
            bIsUsingUdp = cfgAccount["bIsUsingUdp"].as<int>();
            bIsMulticast = cfgAccount["bIsMulticast"].as<int>();
        }
        else {
            //spdlog::info("节点account不存在");
            std::cout << "节点account不存在" << std::endl;
            throw std::runtime_error("节点account不存在.");
        }
        //解析sys
        const YAML::Node& cfgSys = configNode["sys"];
        if (cfgSys) {
            REDIS_HOST = cfgSys["REDIS_HOST"].as<std::string>();
            CZCE = cfgSys["CZCE"].as<std::string>();
            REDIS_DB = cfgSys["REDIS_DB"].as<int>();
            REDIS_PORT = cfgSys["REDIS_PORT"].as<int>();
            REDIS_POOL_SIZE = cfgSys["REDIS_POOL_SIZE"].as<int>();
            REDIS_RECONNECT_TIMES = cfgSys["REDIS_RECONNECT_TIMES"].as<int>();
            REDIS_INTERVAL_SECONDES = cfgSys["REDIS_INTERVAL_SECONDES"].as<int>();
            WEBSOCKET_PORT = cfgSys["WEBSOCKET_PORT"].as<int>();
            HEART_INTERVAL = cfgSys["HEART_INTERVAL"].as<int>();
            LOG_LEVEL = cfgSys["LOG_LEVEL"].as<int>();
        }
        ////解析sys
        //const YAML::Node& cfgSys = configNode["sys"];
        //if (cfgSys) {
        //    REDIS_HOST = cfgSys["REDIS_HOST"].as<std::string>();
        //    REDIS_DB = cfgSys["REDIS_DB"].as<int>();
        //    REDIS_PORT = cfgSys["REDIS_PORT"].as<int>();
        //    REDIS_POOL_SIZE = cfgSys["REDIS_POOL_SIZE"].as<int>();
        //    REDIS_RECONNECT_TIMES = cfgSys["REDIS_RECONNECT_TIMES"].as<int>();
        //    REDIS_INTERVAL_SECONDES = cfgSys["REDIS_INTERVAL_SECONDES"].as<int>();
        //    WEBSOCKET_PORT = cfgSys["WEBSOCKET_PORT"].as<int>();
        //    LOG_LEVEL = cfgSys["LOG_LEVEL"].as<int>();
        //}
        else {
            //spdlog::info("节点sys不存在");
            std::cout << "节点sys不存在" << std::endl;
            throw std::runtime_error("节点sys不存在.");
        }
    }
    else {
        //spdlog::info("配置文件加载失败: {}", filename);
        std::cout << "配置文件加载失败:"+filename << std::endl;
        throw std::runtime_error("配置文件无效.");
    }
}