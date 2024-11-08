#pragma once
#ifndef TRADER_CLIENT_H
#define TRADER_CLIENT_H
#include "ThostFtdcTraderApi.h"
#include <uWebSockets/App.h>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <string>
#include <unordered_set>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <windows.h>
#include <vector>

using namespace uWS;
using json = nlohmann::json;

struct PerSocketData {
    // 可以在这里添加需要的成员变量
    int id;
};

// 下单请求参数结构体
struct OrderRequest {
    std::string instrumentID; // 合约代码
    std::string exchangeID;  // 交易所代码
    int direction;           // 买卖方向
    int offsetFlag;         // 开平标志
    double limitPrice;      // 价格
    int volumeTotalOriginal; // 委托数量
};
//大写是官方，小写是自定义
class TraderClient : public CThostFtdcTraderSpi {
    
private:
    CThostFtdcTraderApi* api;
    std::shared_ptr<WebSocket<false, true, PerSocketData>> ws; //暂时单用户
    bool useWs=true; // 控制是否通过 WebSocket 发送消息,暂时都放行
    std::queue<std::string> messageQueue;
    std::mutex queueMutex;
    std::condition_variable condVar;
    std::thread workerThread;
    int frontID, sessionID; //终端和会话ID
    bool running;
    int nRequestID = 0;
    std::mutex wsMutex; // 用于保护WebSocket连接的互斥锁
    std::map<std::string, CThostFtdcInstrumentCommissionRateField> instrumentCommissionRateMap;
    int OrderRef_num = 1;
    // 函数：生成订单引用编号
    std::string generateOrderRef(int OrderRef_num);
    // 四舍五入到指定的小数位数
    double roundToPrecision(double value, int precision);

    // 获取交易所ID的函数

    std::string getInstrumentId(const std::string& instrumentID) {
        size_t dotPosition = instrumentID.find('.');
        if (dotPosition != std::string::npos) {
            // 找到点号，分割字符串并返回第二个部分
            return instrumentID.substr(dotPosition + 1);
        }
        else {
            // 没有找到点号，返回原字符串
            return instrumentID;
        }
    }
    std::string getExchangeIdByInstrumentId(const std::string& instrumentID) {
        // 检查合约代码中是否包含点字符
        size_t pos = instrumentID.find('.');
        if (pos != std::string::npos) {
            // 如果包含点字符，则取点字符前面的部分作为交易所ID
            return instrumentID.substr(0, pos);
        }
        else {
            // 如果不包含点字符，则根据AppConfig中的配置判断交易所ID
            if (instrumentID.find(AppConfig::CZCE) != std::string::npos) {
                return "CZCE";
            }
            else {
                return AppConfig::ExchangeID; // 默认返回SHHE
            }
        }
    }

    // 报单状态中文映射
    std::unordered_map<TThostFtdcOrderStatusType, std::string> orderStatusMap = {
        {THOST_FTDC_OST_AllTraded, "全部成交"},
        {THOST_FTDC_OST_PartTradedQueueing, "部分成交还在队列中"},
        {THOST_FTDC_OST_PartTradedNotQueueing, "部分成交不在队列中"},
        {THOST_FTDC_OST_NoTradeQueueing, "未成交还在队列中"},
        {THOST_FTDC_OST_NoTradeNotQueueing, "未成交不在队列中"},
        {THOST_FTDC_OST_Canceled, "撤单"},
        {THOST_FTDC_OST_Unknown, "未知"},
        {THOST_FTDC_OST_NotTouched, "尚未触发"},
        {THOST_FTDC_OST_Touched, "已触发"}
    };

   

    // 回调函数声明
    ///报单通知
    virtual void OnRtnOrder(CThostFtdcOrderField* pOrder) override;
    virtual void OnRspQryInvestorPosition(CThostFtdcInvestorPositionField* pInvestorPosition, CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;

    void sendLimitOrder(const char* instrumentID, double price, int volume, char direction, double stopPrice, double profitPrice);
    
    virtual void OnRspOrderAction(CThostFtdcInputOrderActionField* pInputOrderAction, CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;

    ///当客户端与交易后台建立起通信连接时（还未登录前），该方法被调用。
    virtual void OnFrontConnected();
    
    ///客户端认证响应
    virtual void OnRspAuthenticate(CThostFtdcRspAuthenticateField* pRspAuthenticateField, CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast);
    ///登录请求响应
    virtual void OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin, CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast);
    ///请求查询合约手续费率响应
    virtual void OnRspQryInstrumentCommissionRate(CThostFtdcInstrumentCommissionRateField* pInstrumentCommissionRate, CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast);
    ///报单录入请求响应
    virtual void OnRspOrderInsert(CThostFtdcInputOrderField* pInputOrder, CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast);
    ///报单录入错误回报
    virtual void OnErrRtnOrderInsert(CThostFtdcInputOrderField* pInputOrder, CThostFtdcRspInfoField* pRspInfo);
    
    ///请求查询合约响应
    virtual void OnRspQryInstrument(CThostFtdcInstrumentField* pInstrument, CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast);
    

    ///投资者结算结果确认响应
    virtual void OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField* pSettlementInfoConfirm,
        CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast);
   
    

    
    void wsSendErrMsg(CThostFtdcRspInfoField* pRspInfo, const std::string& type);


    ///请求查询资金账户响应
    virtual void OnRspQryTradingAccount(CThostFtdcTradingAccountField* pTradingAccount, CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast);



public:
    TraderClient();
    void heartbeatThread();
    virtual ~TraderClient();

    //交易、账户相关：
    double available; //可用余额
    double balance; //权益
    //费率相关
    //持仓相关：
    json positionArr;
    json instrumentIdArr;


    // 将GB2312编码的字符串转换为UTF-8编码
    std::string GB2312ToUTF8(const char* gb2312Str) {
        int wideLength = MultiByteToWideChar(CP_ACP, 0, gb2312Str, -1, NULL, 0);
        std::vector<wchar_t> wideStr(wideLength);
        MultiByteToWideChar(CP_ACP, 0, gb2312Str, -1, &wideStr[0], wideLength);

        int utf8Length = WideCharToMultiByte(CP_UTF8, 0, &wideStr[0], -1, NULL, 0, NULL, NULL);
        std::vector<char> utf8Str(utf8Length);
        WideCharToMultiByte(CP_UTF8, 0, &wideStr[0], -1, &utf8Str[0], utf8Length, NULL, NULL);

        return std::string(utf8Str.begin(), utf8Str.end());
    }

    
    void init(const char* frontAddress);
    void init();

    void startWorkerThread();
    void stopWorkerThread();
    void enqueueMessage(const std::string& message);

    void sendWebSocketMessage(const std::string& message) {// 避免Cork Buffer问题
        std::lock_guard<std::mutex> lock(wsMutex);
        if (useWs && ws) {
            ws->send(message.data(), uWS::OpCode::TEXT);
        }
    }
    //登入
    void login();
    void OnFrontDisconnected(int nReason);
    //验证终端
    void authenticate();
    //查询单个合同费率
    int qryInstrumentCommissionRate(const std::string& instrumentID);
    //查询账户
    int qryTradingAccount();
    //预埋单录入 - 限价单
    void parkedOrderInsert(double limitPrice, char direction, int volume,const std::string& instrumentID);
    //查询持仓汇总
    void qryInvestorPosition();
    //市价成交,开平 bs oc
    void sendOrderByAnyPrice(const json& params); 
    //限价止损单
    void sendOrderTouch(const json& params);
    //取品种
    void fetchInstrumentIdsFromRedis();

    //请求查询合约
    void qryInstrument(const json& params);

    /// <summary>
    /// 确认结算单
    /// </summary>
    void confirmSettlementInfo();
    // WebSocket 回调
    void onOpen(WebSocket<false,true,PerSocketData>* ws);
    void onMessage(WebSocket<false,true,PerSocketData>* ws, std::string_view message, OpCode opCode);
    void onClose(WebSocket<false,true,PerSocketData>* ws, int code, std::string_view reason);
    
};
#endif // TRADER_CLIENT_H