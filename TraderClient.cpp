#include "stdafx.h"
#include "TraderClient.h"
#include "AppConfig.h"
#include "DataCollect.h"
#include "RedisUtil.h"
#include <ctime>
#include <nlohmann/json.hpp>
using json = nlohmann::json;
TraderClient::TraderClient() : api(nullptr), ws(nullptr), useWs(true), running(false) {
    // 启动心跳后台线程
    std::thread heartbeatThreadObj(&TraderClient::heartbeatThread, this);
    heartbeatThreadObj.detach(); // 将线程分离，使其在后台运行
}


// 后台线程函数
void TraderClient::heartbeatThread() {
    while (1) {
        // 发送心跳消息
        sendWebSocketMessage("无相");
        std::this_thread::sleep_for(std::chrono::seconds(AppConfig::HEART_INTERVAL));
    }
}


TraderClient::~TraderClient() {
    stopWorkerThread();
    if (api) {
        api->Release();
        //delete api;
        api = nullptr;
    }
}

void TraderClient::init(const char* frontAddress) {
    spdlog::info("api版本: {}", api->GetApiVersion());
    spdlog::info("采集库版本：{}", CTP_GetDataCollectApiVersion());
    api = CThostFtdcTraderApi::CreateFtdcTraderApi(".\\flow\\");
    api->RegisterSpi(this);
    api->RegisterFront((char*)frontAddress);
    api->SubscribePrivateTopic(THOST_TERT_QUICK);
    api->SubscribePublicTopic(THOST_TERT_QUICK);
    api->Init();
    authenticate();
    // 登录逻辑...
}

void TraderClient::init() {
    init(AppConfig::FrontAddr.c_str());
}

void TraderClient::startWorkerThread() {
    running = true;
    workerThread = std::thread([this]() {
        while (running) {
            spdlog::debug("监听BS信号线程");
            std::unique_lock<std::mutex> lock(queueMutex);
            condVar.wait(lock, [this] { return !messageQueue.empty() || !running; });
            if (!running) break;
            std::string message = messageQueue.front();
            messageQueue.pop();
            lock.unlock();
            sendWebSocketMessage(message.data());
           
        }
        });
}

void TraderClient::stopWorkerThread() {
    running = false;
    condVar.notify_one();
    if (workerThread.joinable()) {
        workerThread.join();
    }
}

void TraderClient::enqueueMessage(const std::string& message) {
    std::lock_guard<std::mutex> lock(queueMutex);
    messageQueue.push(message);
    condVar.notify_one();
}





std::string TraderClient::generateOrderRef(int OrderRef_num) {
    std::time_t t = std::time(nullptr);
    std::tm* now = std::localtime(&t);
    std::stringstream ss;
    ss << (now->tm_year + 1900) % 100 << // 取年份的后两位
        std::setfill('0') << std::setw(2) << (now->tm_mon + 1) << // 月份
        std::setfill('0') << std::setw(2) << now->tm_mday; // 日期
    ss << std::setfill('0') << std::setw(5) << OrderRef_num; // 订单号
    return ss.str();
}



double TraderClient::roundToPrecision(double value, int precision) {
    double scale = std::pow(10.0, precision);
    return std::round(value * scale) / scale;
}


//void TraderClient::OnRtnTrade(CThostFtdcTradeField* pTrade) {
//    std::string message = "新的交易回报";
//    enqueueMessage(message);
//}


void TraderClient::OnRspOrderAction(CThostFtdcInputOrderActionField* pInputOrderAction, CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) {
    std::string message = "撤单操作结果";
    enqueueMessage(message);
}

void TraderClient::onOpen(WebSocket<false,true,PerSocketData>* ws) {
    spdlog::info("WebSocket客户端连接");
    {
        std::lock_guard<std::mutex> lock(wsMutex); // 加锁以保护共享资源ws
        this->ws = std::shared_ptr<WebSocket<false, true, PerSocketData>>(ws, [](WebSocket<false, true, PerSocketData>* p) {
            // 自定义删除器（如果需要的话）
            // 通常情况下，不需要自定义删除器，因为默认的删除器会调用delete
            });


    }
}

void TraderClient::onMessage(WebSocket<false,true,PerSocketData>* ws, std::string_view message, OpCode opCode) {
    spdlog::info("收到WebSocket消息：{}", message);
    //处理前端消息：
    try {
        
        json params = json::parse(message);
        std::string type = params.at("type").get<std::string>();

        // 根据 type 字段调用相应的方法
        if (type == "qryInvestorPosition") {
            qryInvestorPosition();
        }

        // 根据 type 字段调用相应的方法
        if (type == "sendOrderByAnyPrice") {
            sendOrderByAnyPrice(params);
        } if (type == "qryInstrument") {
            qryInstrument(params);
        }
        else {
            spdlog::info("未知的请求类型：【{}】", type);
        }
    }catch (const std::exception& e) {
        spdlog::error("错误：{}", e.what());
    }
    catch (...) {
        spdlog::error("未知错误");
    }
}

void TraderClient::onClose(WebSocket<false,true,PerSocketData>* ws, int code, std::string_view reason) {
    spdlog::info("WebSocket客户端断开连接：{} {}", code, reason);
    std::lock_guard<std::mutex> lock(wsMutex); // 加锁以保护共享资源
    // 如果有其他线程可能在此时访问 ws，确保它们通过锁同步
    this->ws.reset(); // 使用 reset() 来减少引用计数，可能释放 WebSocket 对象
    //this->ws = nullptr;
}
//客户端认证

void TraderClient::OnFrontConnected()
{
    spdlog::info("连接成功");
    authenticate();
}

void TraderClient::OnFrontDisconnected(int nReason)
{
    spdlog::info("<OnFrontDisconnected>\n");
    spdlog::error("连接丢失，可能需要重新初始化: {}", nReason);
    spdlog::info("</OnFrontDisconnected>\n");
}
void TraderClient::authenticate()
{
    CThostFtdcReqAuthenticateField a = { 0 };
    strcpy_s(a.BrokerID, AppConfig::BrokerID.c_str());
    strcpy_s(a.UserID, AppConfig::UserID.c_str());
    strcpy_s(a.AuthCode, AppConfig::AuthCode.c_str());
    strcpy_s(a.AppID, AppConfig::AppID.c_str());
    //strcpy_s(a.UserProductInfo, g_chAppID);
    int b = api->ReqAuthenticate(&a, nRequestID++);
    spdlog::info("\t客户端认证 = [{}]\n", b);
}

void TraderClient::OnRspAuthenticate(CThostFtdcRspAuthenticateField* pRspAuthenticateField, CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast)
{
    spdlog::info("<OnRspAuthenticate>");

    if (pRspAuthenticateField)
    {
        spdlog::info("\tBrokerID [{}]", pRspAuthenticateField->BrokerID);
        spdlog::info("\tUserID [{}]", pRspAuthenticateField->UserID);
        spdlog::info("\tUserProductInfo [{}]", pRspAuthenticateField->UserProductInfo);
        spdlog::info("\tAppID [{}]", pRspAuthenticateField->AppID);
        spdlog::info("\tAppType [{}]", pRspAuthenticateField->AppType);
    }
    spdlog::info("\tnRequestID [{}]", nRequestID);
    spdlog::info("\tbIsLast [{}]", bIsLast);
    if (pRspInfo)
    {
        if (pRspInfo->ErrorID == 0) {
            login();
        }
        else
        {
            wsSendErrMsg(pRspInfo,"login");
        }
    }
    spdlog::info("</OnRspAuthenticate>");
}

void TraderClient::OnRspQryTradingAccount(CThostFtdcTradingAccountField* pTradingAccount, CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast){
    try {
        spdlog::info("查询账户余额响应：");
        if (pTradingAccount) {
            spdlog::info("账户ID: {}; 可用资金: {:.2f}; 账户余额: {:.2f}",
                pTradingAccount->AccountID, pTradingAccount->Available, pTradingAccount->Balance);
            available = pTradingAccount->Available;
            balance = pTradingAccount->Balance;
            if (useWs && ws) {
                nlohmann::json json_response;
                json_response["success"] = true;
                json_response["data"]["available"] = std::round(pTradingAccount->Available * 100.0) / 100.0;
                json_response["data"]["balance"] = pTradingAccount->Balance;
                json_response["type"] = "qryTradingAccount";
                std::string jsonstr = json_response.dump();
                sendWebSocketMessage(jsonstr);
            }
        }
        if (pRspInfo) {
            spdlog::info("\tErrorMsg [{}]", GB2312ToUTF8(pRspInfo->ErrorMsg));
            spdlog::info("\tErrorID [{}]", pRspInfo->ErrorID);
            if (useWs && ws) {
                json json_response;
                json_response["success"] = false;
                json_response["data"]["ErrorID"] = pRspInfo->ErrorID;
                json_response["data"]["ErrorMsg"] = GB2312ToUTF8(pRspInfo->ErrorMsg);
                json_response["type"] = "qryTradingAccount";
                std::string jsonstr = json_response.dump();
                sendWebSocketMessage(jsonstr);
            }
            
        }
        
    }
    catch (const std::exception& e) {
        spdlog::error("异常：{}", e.what());
    }
}

void TraderClient::login()
{
    CThostFtdcReqUserLoginField reqUserLogin = { 0 };
    strcpy_s(reqUserLogin.BrokerID, AppConfig::BrokerID.c_str());
    strcpy_s(reqUserLogin.UserID, AppConfig::UserID.c_str());
    strcpy_s(reqUserLogin.Password, AppConfig::Password.c_str());
    //strcpy_s(reqUserLogin.UserProductInfo, AppConfig::AppID.c_str());
    //strcpy_s(reqUserLogin.ClientIPAddress, "::c0a8:0101");
    strcpy_s(reqUserLogin.LoginRemark, "test");
    // 发出登陆请求
    int b = api->ReqUserLogin(&reqUserLogin, nRequestID++);
    spdlog::info("网络情况：{}", b);
}

void TraderClient::OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin, CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast)
{
    spdlog::info("<OnRspUserLogin>");

    if (pRspUserLogin)
    {
        spdlog::debug("\t交易日 [{}]", pRspUserLogin->TradingDay);
        spdlog::debug("\t登录成功时间 [{}]", pRspUserLogin->LoginTime);
        spdlog::debug("\t经纪公司代码 [{}]", pRspUserLogin->BrokerID);
        spdlog::debug("\t用户代码 [{}]", pRspUserLogin->UserID);
        spdlog::debug("\t交易系统名称 [{}]", pRspUserLogin->SystemName);
        spdlog::debug("\t最大报单引用 [{}]", pRspUserLogin->MaxOrderRef);
        spdlog::debug("\t上期所时间 [{}]", pRspUserLogin->SHFETime);
        spdlog::debug("\t大商所时间 [{}]", pRspUserLogin->DCETime);
        spdlog::debug("\t郑商所时间 [{}]", pRspUserLogin->CZCETime);
        spdlog::debug("\t中金所时间 [{}]", pRspUserLogin->FFEXTime);
        spdlog::debug("\t能源中心时间 [{}]", pRspUserLogin->INETime);
        spdlog::debug("\t后台版本信息 [{}]", pRspUserLogin->SysVersion);
        spdlog::debug("\t广期所时间 [{}]", pRspUserLogin->GFEXTime);
        spdlog::debug("\t前置编号 [{}]", pRspUserLogin->FrontID);
        spdlog::debug("\t会话编号 [{}]", pRspUserLogin->SessionID);
        frontID = pRspUserLogin->FrontID;
        sessionID = pRspUserLogin->SessionID;
        json json_response;
        json_response["success"] = true;
        json_response["data"]["FrontID"] = pRspUserLogin->FrontID;
        json_response["data"]["SessionID"] = pRspUserLogin->SessionID;
        json_response["data"]["Message"] = "登入成功";
        json_response["type"] = "login";
        std::string jsonstr = json_response.dump();
        sendWebSocketMessage(jsonstr);
    }
    if (pRspInfo)
    {
        spdlog::info("\tErrorMsg [{}]", GB2312ToUTF8(pRspInfo->ErrorMsg));
        spdlog::info("\tErrorID [{}]", pRspInfo->ErrorID);
        if (pRspInfo->ErrorID == 0) {
            spdlog::info("登入成功！");
            frontID = pRspUserLogin->FrontID;
            sessionID = pRspUserLogin->SessionID;
            // 调用方法，填充instrumentIdArr
            if (instrumentIdArr.empty()) {
                fetchInstrumentIdsFromRedis();
                std::cout << instrumentIdArr.dump(4) << std::endl;
            }

           
            confirmSettlementInfo();//接着确认结算单

        }
        else
        {
            wsSendErrMsg(pRspInfo, "login");
        }
    }
    spdlog::info("\tnRequestID [{}]", nRequestID);
    spdlog::info("\tbIsLast [{}]", bIsLast);
    spdlog::info("</OnRspUserLogin>");
}



void TraderClient::wsSendErrMsg(CThostFtdcRspInfoField* pRspInfo, const std::string& type)
{
    if (pRspInfo)
    {
        spdlog::info("\t错误信息 [{}]", GB2312ToUTF8(pRspInfo->ErrorMsg));
        spdlog::info("\t错误ID [{}]", pRspInfo->ErrorID);
        if (useWs && ws) {
            json json_response;
            json_response["success"] = false;
            json_response["data"]["ErrorID"] = pRspInfo->ErrorID;
            json_response["data"]["ErrorMsg"] = GB2312ToUTF8(pRspInfo->ErrorMsg);
            json_response["type"] = type;
            std::string jsonstr = json_response.dump();
            sendWebSocketMessage(jsonstr);
        }

    }
   
}

// 查询费用的请求方法
int TraderClient::qryInstrumentCommissionRate(const std::string& instrumentID) {
    CThostFtdcQryInstrumentCommissionRateField qryInfo = {0};
    memset(&qryInfo, 0, sizeof(qryInfo)); // 初始化结构体

    // 使用AppConfig的静态成员配置请求信息
    strcpy_s(qryInfo.BrokerID, AppConfig::BrokerID.c_str());
    strcpy_s(qryInfo.InvestorID, AppConfig::UserID.c_str()); // 假设这里InvestorID与UserID相同
    strcpy_s(qryInfo.ExchangeID, AppConfig::ExchangeID.c_str()); // 假设存在ExchangeID配置
    strcpy_s(qryInfo.InvestUnitID, AppConfig::UserProductInfo.c_str()); // 假设存在UserProductInfo配置
    strcpy_s(qryInfo.InstrumentID, instrumentID.c_str());

    // 日志记录
    spdlog::debug("<查询合约手续费率>\n");
    spdlog::debug("\t经纪公司代码 [{}]\n", qryInfo.BrokerID);
    spdlog::debug("\t投资者代码 [{}]\n", qryInfo.InvestorID);
    spdlog::debug("\t保留字段1 [{}]\n", qryInfo.reserve1);
    spdlog::debug("\t交易所代码 [{}]\n", qryInfo.ExchangeID);
    spdlog::debug("\t投资单元代码 [{}]\n", qryInfo.InvestUnitID);
    spdlog::debug("\t合约代码 [{}]\n", qryInfo.InstrumentID);


    // 假设nRequestID是一个预先定义的请求ID
    //spdlog::info("\tnRequestID [{}]\n", nReq++);
    spdlog::info("</ReqQryInstrumentCommissionRate>\n");

    // 发送请求
    return api->ReqQryInstrumentCommissionRate(&qryInfo, nRequestID++);
}

void TraderClient::OnRspQryInstrumentCommissionRate(CThostFtdcInstrumentCommissionRateField* pInstrumentCommissionRate, CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast)
{
    spdlog::info("<OnRspQryInstrumentCommissionRate>");
    if (pInstrumentCommissionRate)
    {
        spdlog::debug("\treserve1 [{}]", GB2312ToUTF8(pInstrumentCommissionRate->reserve1));
        spdlog::debug("\t投资者范围 [{}]", pInstrumentCommissionRate->InvestorRange);
        spdlog::debug("\t经纪公司代码 [{}]", GB2312ToUTF8(pInstrumentCommissionRate->BrokerID));
        spdlog::debug("\t投资者代码 [{}]", GB2312ToUTF8(pInstrumentCommissionRate->InvestorID));
        spdlog::debug("\t开仓手续费率 [{}]", pInstrumentCommissionRate->OpenRatioByMoney);
        spdlog::debug("\t开仓手续费 [{}]", pInstrumentCommissionRate->OpenRatioByVolume);
        spdlog::debug("\t平仓手续费率 [{}]", pInstrumentCommissionRate->CloseRatioByMoney);
        spdlog::debug("\t平仓手续费 [{}]", pInstrumentCommissionRate->CloseRatioByVolume);
        spdlog::debug("\t平今手续费率 [{}]", pInstrumentCommissionRate->CloseTodayRatioByMoney);
        spdlog::debug("\t平今手续费 [{}]", pInstrumentCommissionRate->CloseTodayRatioByVolume);
        spdlog::debug("\t交易所代码 [{}]", GB2312ToUTF8(pInstrumentCommissionRate->ExchangeID));
        spdlog::debug("\t业务类型 [{}]", pInstrumentCommissionRate->BizType);
        spdlog::debug("\t投资单元代码 [{}]", GB2312ToUTF8(pInstrumentCommissionRate->InvestUnitID));
        spdlog::debug("\t合约代码 [{}]", GB2312ToUTF8(pInstrumentCommissionRate->InstrumentID));

        instrumentCommissionRateMap[pInstrumentCommissionRate->InstrumentID] = *pInstrumentCommissionRate;
    }
    wsSendErrMsg(pRspInfo, "qryInstrumentCommissionRate");

    spdlog::info("\t请求ID [{}]", nRequestID);
    spdlog::info("\t是否最后一条 [{}]", bIsLast ? "是" : "否");
    spdlog::info("</OnRspQryInstrumentCommissionRate>");
}

int TraderClient::qryTradingAccount()
{
    CThostFtdcQryTradingAccountField qryTradingAccount = {0};
    CThostFtdcQryTradingAccountField* pQryTradingAccount = &qryTradingAccount;
    strcpy_s(pQryTradingAccount->BrokerID, AppConfig::BrokerID.c_str());
    strcpy_s(pQryTradingAccount->InvestorID, AppConfig::UserID.c_str()); // 假设这里 InvestorID 与 UserID 相同
    strcpy_s(pQryTradingAccount->CurrencyID, AppConfig::CurrencyID.c_str()); // 假设存在 CurrencyID 配置
    //strcpy_s(pQryTradingAccount->AccountID, AppConfig::AccountID.c_str()); // 假设存在 AccountID 配置
    //pQryTradingAccount->BizType = AppConfig::BizType; // 假设存在 BizType 配置

    // 日志记录
    spdlog::debug("<请求查询交易账户>\n");
    
    spdlog::debug("\t请求ID {}\n", nRequestID);
    spdlog::debug("</请求查询交易账户>\n");
    int res= api->ReqQryTradingAccount(pQryTradingAccount, nRequestID++);
    return res;
};



/// 预埋单录入 - 限价单
void TraderClient::parkedOrderInsert(double limitPrice, char direction, int volume, const std::string& instrumentID) {
    CThostFtdcParkedOrderField parkedOrder = { 0 }; // 创建预埋单结构体并初始化

    strcpy_s(parkedOrder.BrokerID, AppConfig::BrokerID.c_str()); // 经纪公司代码
    strcpy_s(parkedOrder.InvestorID, AppConfig::InvestorID.c_str()); // 投资者代码
    strcpy_s(parkedOrder.InstrumentID, getInstrumentId(instrumentID).c_str()); // 合约代码
    strcpy_s(parkedOrder.UserID, AppConfig::UserID.c_str()); // 用户代码
    strcpy_s(parkedOrder.ExchangeID, getExchangeIdByInstrumentId(instrumentID).c_str()); // 交易所代码

    std::string orderRef = generateOrderRef(OrderRef_num++); // 生成订单引用编号
    strcpy_s(parkedOrder.OrderRef, orderRef.c_str()); // 报单引用

    parkedOrder.OrderPriceType = THOST_FTDC_OPT_LimitPrice; // 报单价格条件
    parkedOrder.Direction = direction; // 买卖方向///买THOST_FTDC_D_Buy '0' 卖THOST_FTDC_D_Sell '1'
    parkedOrder.CombOffsetFlag[0] = THOST_FTDC_OF_Open; // 组合开平标志
    parkedOrder.CombHedgeFlag[0] = THOST_FTDC_HF_Speculation; // 组合投机套保标志
    parkedOrder.LimitPrice = limitPrice; // 价格
    parkedOrder.VolumeTotalOriginal = volume; // 数量
    parkedOrder.TimeCondition = THOST_FTDC_TC_GFD; // 有效期类型
    parkedOrder.VolumeCondition = THOST_FTDC_VC_AV; // 成交量类型
    parkedOrder.MinVolume = 1; // 最小成交量
    parkedOrder.ContingentCondition = THOST_FTDC_CC_Immediately; // 触发条件：立即
    parkedOrder.StopPrice = 0; // 止损价
    parkedOrder.ForceCloseReason = THOST_FTDC_FCC_NotForceClose; // 强平原因
    parkedOrder.IsAutoSuspend = 0; // 自动挂起标志

    int result = api->ReqParkedOrderInsert(&parkedOrder, nRequestID++); // 发送预埋单录入请求

    spdlog::debug(R"(
                                        请求限价预埋单：
                                        经纪公司代码：{}\n
                                        投资者代码：{}\n
                                        合约代码：{}\n
                                        用户代码：{}\n
                                        交易所代码：{}\n
                                        报单引用：{}\n
                                        报单价格条件：{}\n
                                        买卖方向：{}\n
                                        开平标志：{}\n
                                        投机套保标志：{}\n
                                        价格：{}\n
                                        数量：{}\n
                                        有效期类型：{}\n
                                        成交量类型：{}\n
                                        最小成交量：{}\n
                                        触发条件：{}\n
                                        止损价：{}\n
                                        强平原因：{}\n
                                        自动挂起标志：{}\n
                                        发送结果：{}\n
)",
                                        parkedOrder.BrokerID,
                                        parkedOrder.InvestorID,
                                        parkedOrder.InstrumentID,
                                        parkedOrder.UserID,
                                        parkedOrder.ExchangeID,
                                        orderRef,
                                        parkedOrder.OrderPriceType, // 注意：这里需要转换为中文描述
                                        parkedOrder.Direction, // 注意：这里需要转换为中文描述
                                        parkedOrder.CombOffsetFlag[0], // 注意：这里需要转换为中文描述
                                        parkedOrder.CombHedgeFlag[0], // 注意：这里需要转换为中文描述
                                        parkedOrder.LimitPrice,
                                        parkedOrder.VolumeTotalOriginal,
                                        parkedOrder.TimeCondition, // 注意：这里需要转换为中文描述
                                        parkedOrder.VolumeCondition, // 注意：这里需要转换为中文描述
                                        parkedOrder.MinVolume,
                                        parkedOrder.ContingentCondition, // 注意：这里需要转换为中文描述
                                        parkedOrder.StopPrice,
                                        parkedOrder.ForceCloseReason, // 注意：这里需要转换为中文描述
                                        parkedOrder.IsAutoSuspend,
                                        (result == 0) ? "发送成功" : "发送失败，错误码=" + std::to_string(result)
                                        );

}

void TraderClient::OnRspOrderInsert(CThostFtdcInputOrderField* pInputOrder, CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) {
    spdlog::info("<OnRspOrderInsert>");
    if (pInputOrder) {
        spdlog::debug("\t经纪公司代码 [{}]", GB2312ToUTF8(pInputOrder->BrokerID));
        spdlog::debug("\t投资者代码 [{}]", GB2312ToUTF8(pInputOrder->InvestorID));
        spdlog::debug("\t保留的无效字段 [{}]", GB2312ToUTF8(pInputOrder->reserve1));
        spdlog::debug("\t报单引用 [{}]", GB2312ToUTF8(pInputOrder->OrderRef));
        spdlog::debug("\t用户代码 [{}]", GB2312ToUTF8(pInputOrder->UserID));
        spdlog::debug("\t报单价格条件 [{}]", pInputOrder->OrderPriceType);
        spdlog::debug("\t买卖方向 [{}]", pInputOrder->Direction);
        spdlog::debug("\t组合开平标志 [{}]", GB2312ToUTF8(pInputOrder->CombOffsetFlag));
        spdlog::debug("\t组合投机套保标志 [{}]", GB2312ToUTF8(pInputOrder->CombHedgeFlag));
        spdlog::debug("\t价格 [{}]", pInputOrder->LimitPrice);
        spdlog::debug("\t数量 [{}]", pInputOrder->VolumeTotalOriginal);
        spdlog::debug("\t有效期类型 [{}]", pInputOrder->TimeCondition);
        spdlog::debug("\tGTD日期 [{}]", GB2312ToUTF8(pInputOrder->GTDDate));
        spdlog::debug("\t成交量类型 [{}]", pInputOrder->VolumeCondition);
        spdlog::debug("\t最小成交量 [{}]", pInputOrder->MinVolume);
        spdlog::debug("\t触发条件 [{}]", pInputOrder->ContingentCondition);
        spdlog::debug("\t止损价 [{}]", pInputOrder->StopPrice);
        spdlog::debug("\t强平原因 [{}]", pInputOrder->ForceCloseReason);
        spdlog::debug("\t自动挂起标志 [{}]", pInputOrder->IsAutoSuspend);
        spdlog::debug("\t业务单元 [{}]", GB2312ToUTF8(pInputOrder->BusinessUnit));
        spdlog::debug("\t请求编号 [{}]", pInputOrder->RequestID);
        spdlog::debug("\t用户强评标志 [{}]", pInputOrder->UserForceClose);
        spdlog::debug("\t互换单标志 [{}]", pInputOrder->IsSwapOrder);
        spdlog::debug("\t交易所代码 [{}]", GB2312ToUTF8(pInputOrder->ExchangeID));
        spdlog::debug("\t投资单元代码 [{}]", GB2312ToUTF8(pInputOrder->InvestUnitID));
        spdlog::debug("\t资金账号 [{}]", GB2312ToUTF8(pInputOrder->AccountID));
        spdlog::debug("\t币种代码 [{}]", GB2312ToUTF8(pInputOrder->CurrencyID));
        spdlog::debug("\t交易编码 [{}]", GB2312ToUTF8(pInputOrder->ClientID));
        spdlog::debug("\t保留的无效字段 [{}]", GB2312ToUTF8(pInputOrder->reserve2));
        spdlog::debug("\tMac地址 [{}]", GB2312ToUTF8(pInputOrder->MacAddress));
        spdlog::debug("\t合约代码 [{}]", GB2312ToUTF8(pInputOrder->InstrumentID));
        spdlog::debug("\tIP地址 [{}]", GB2312ToUTF8(pInputOrder->IPAddress));
    }
    wsSendErrMsg(pRspInfo, "rspOrderInsert");
    spdlog::info("\t请求ID [{}]", nRequestID);
    spdlog::info("\t是否最后一条 [{}]", bIsLast ? "是" : "否");
    spdlog::info("</OnRspOrderInsert>");
}

void TraderClient::qryInvestorPosition() {
    CThostFtdcQryInvestorPositionField req = { 0 };

    // 使用 AppConfig 类的静态成员来设置请求字段
    strcpy_s(req.BrokerID, AppConfig::BrokerID.c_str());
    strcpy_s(req.InvestorID, AppConfig::InvestorID.c_str());

    //strcpy_s(req.InstrumentID, AppConfig::InstrumentID); //不选则是查所有合约

    // 发送请求前清空持仓
    positionArr.clear();
    int ret = api->ReqQryInvestorPosition(&req, nRequestID++);

    // 使用 spdlog 记录日志
    if (ret == 0) {
        spdlog::info("请求查询投资者持仓发送成功");
    }
    else {
        spdlog::error("请求查询投资者持仓发送失败，错误序号=[{}]", ret);
    }
}

void TraderClient::OnRspQryInvestorPosition(CThostFtdcInvestorPositionField* pInvestorPosition, CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast)
{
    spdlog::debug("<OnRspQryInvestorPosition>");

    if (pInvestorPosition)
    {
        spdlog::debug("\t保留的无效字段: {}", pInvestorPosition->reserve1);
        spdlog::debug("\t经纪公司代码: {}", pInvestorPosition->BrokerID);
        spdlog::debug("\t投资者代码: {}", pInvestorPosition->InvestorID);
        spdlog::debug("\t持仓多空方向: {}", pInvestorPosition->PosiDirection);
        spdlog::debug("\t投机套保标志: {}", pInvestorPosition->HedgeFlag);
        spdlog::debug("\t持仓日期: {}", pInvestorPosition->PositionDate);
        spdlog::debug("\t上日持仓: {}", pInvestorPosition->YdPosition);
        spdlog::debug("\t今日持仓: {}", pInvestorPosition->Position);
        spdlog::debug("\t多头冻结: {}", pInvestorPosition->LongFrozen);
        spdlog::debug("\t空头冻结: {}", pInvestorPosition->ShortFrozen);
        spdlog::debug("\t开仓冻结金额: {:.8f}", pInvestorPosition->LongFrozenAmount);
        spdlog::debug("\t开仓冻结金额: {:.8f}", pInvestorPosition->ShortFrozenAmount);
        spdlog::debug("\t开仓量: {}", pInvestorPosition->OpenVolume);
        spdlog::debug("\t平仓量: {}", pInvestorPosition->CloseVolume);
        spdlog::debug("\t开仓金额: {:.8f}", pInvestorPosition->OpenAmount);
        spdlog::debug("\t平仓金额: {:.8f}", pInvestorPosition->CloseAmount);
        spdlog::debug("\t持仓成本: {:.8f}", pInvestorPosition->PositionCost);
        spdlog::debug("\t上次占用的保证金: {:.8f}", pInvestorPosition->PreMargin);
        spdlog::debug("\t占用的保证金: {:.8f}", pInvestorPosition->UseMargin);
        spdlog::debug("\t冻结的保证金: {:.8f}", pInvestorPosition->FrozenMargin);
        spdlog::debug("\t冻结的资金: {:.8f}", pInvestorPosition->FrozenCash);
        spdlog::debug("\t冻结的手续费: {:.8f}", pInvestorPosition->FrozenCommission);
        spdlog::debug("\t资金差额: {:.8f}", pInvestorPosition->CashIn);
        spdlog::debug("\t手续费: {:.8f}", pInvestorPosition->Commission);
        spdlog::debug("\t平仓盈亏: {:.8f}", pInvestorPosition->CloseProfit);
        spdlog::debug("\t持仓盈亏: {:.8f}", pInvestorPosition->PositionProfit);
        spdlog::debug("\t上次结算价: {:.8f}", pInvestorPosition->PreSettlementPrice);
        spdlog::debug("\t本次结算价: {:.8f}", pInvestorPosition->SettlementPrice);
        spdlog::debug("\t交易日: {}", pInvestorPosition->TradingDay);
        spdlog::debug("\t结算编号: {}", pInvestorPosition->SettlementID);
        spdlog::debug("\t开仓成本: {:.8f}", pInvestorPosition->OpenCost);
        spdlog::debug("\t交易所保证金: {:.8f}", pInvestorPosition->ExchangeMargin);
        spdlog::debug("\t组合成交形成的持仓: {}", pInvestorPosition->CombPosition);
        spdlog::debug("\t组合多头冻结: {}", pInvestorPosition->CombLongFrozen);
        spdlog::debug("\t组合空头冻结: {}", pInvestorPosition->CombShortFrozen);
        spdlog::debug("\t逐日盯市平仓盈亏: {:.8f}", pInvestorPosition->CloseProfitByDate);
        spdlog::debug("\t逐笔对冲平仓盈亏: {:.8f}", pInvestorPosition->CloseProfitByTrade);
        spdlog::debug("\t今日持仓: {}", pInvestorPosition->TodayPosition);
        spdlog::debug("\t保证金率: {:.8f}", pInvestorPosition->MarginRateByMoney);
        spdlog::debug("\t保证金率(按手数): {:.8f}", pInvestorPosition->MarginRateByVolume);
        spdlog::debug("\t执行冻结: {}", pInvestorPosition->StrikeFrozen);
        //spdlog::debug("\t执行冻结金额: {:.8f}", pInvestorPosition->StrikeFrozen);
        spdlog::debug("\t交易所代码: {}", pInvestorPosition->ExchangeID);
        spdlog::debug("\t投资单元代码: {}", pInvestorPosition->InvestUnitID);
        spdlog::debug("\t放弃执行冻结: {}", pInvestorPosition->AbandonFrozen);
        spdlog::debug("\t执行冻结的昨仓: {}", pInvestorPosition->YdStrikeFrozen);
        spdlog::debug("\t持仓成本差值: {:.8f}", pInvestorPosition->PositionCostOffset);
        spdlog::debug("\ttas持仓手数: {}", pInvestorPosition->TasPosition);
        spdlog::debug("\ttas持仓成本: {:.8f}", pInvestorPosition->TasPositionCost);
        spdlog::debug("\t合约代码: {}", pInvestorPosition->InstrumentID);


        //std::string message = "持仓查询结果";
        //enqueueMessage(message);
        if (useWs && ws) {
            json positionItem;
            positionItem["ContractName"] = pInvestorPosition->InstrumentID; // 合约代码，假设前端需要合约名称，这里直接使用合约代码
            positionItem["PositionDate"] = pInvestorPosition->PositionDate; // 持仓日期
            // 持仓多空方向转换
            std::string direction = pInvestorPosition->PosiDirection == '1' ? "净" :
                pInvestorPosition->PosiDirection == '2' ? "多" :
                pInvestorPosition->PosiDirection == '3' ? "空" : "未知";
            positionItem["PosiDirection"] = direction; // 持仓多空方向
            positionItem["Position"] = pInvestorPosition->Position; // 可用持仓量
            positionItem["OpenCost"] = roundToPrecision(pInvestorPosition->OpenCost, 2); // 开仓均价
            positionItem["PositionProfit"] = roundToPrecision(pInvestorPosition->PositionProfit, 2); // 逐笔浮盈，应该前端根据报价实时计算
            positionItem["ProfitRatio"] = roundToPrecision((pInvestorPosition->PositionProfit / pInvestorPosition->PositionCost) * 100, 2); // 盈利价差
            positionItem["TotalValue"] = roundToPrecision(pInvestorPosition->Position * pInvestorPosition->SettlementPrice, 2); // 合约总价值
            positionItem["UseMargin"] = roundToPrecision(pInvestorPosition->UseMargin, 2); // 保证金
            positionItem["CloseProfitByDate"] = roundToPrecision(pInvestorPosition->CloseProfitByDate, 2); // 盯市浮盈
            positionItem["ExchangeID"] = pInvestorPosition->ExchangeID; // 盯市浮盈
            positionArr.push_back(positionItem);

            if (bIsLast) {
                json json_response;
                json_response["success"] = true; 
                json_response["data"] = positionArr;
                json_response["type"] = "qryInvestorPosition";// 设置响应类型为 "qryInvestorPosition"
                // 将 JSON 对象转换为字符串并发送
                std::string jsonstr = json_response.dump();
                sendWebSocketMessage(jsonstr);
            }
           
        }
    }
    
    wsSendErrMsg(pRspInfo, "qryInvestorPosition");
    spdlog::debug("\t请求ID: {}", nRequestID);
    spdlog::debug("\t是否最后一条: {}", bIsLast);
    spdlog::debug("</OnRspQryInvestorPosition>");
}
void TraderClient::sendLimitOrder(const char* instrumentID, double price, int volume, char direction, double stopPrice, double profitPrice) {
    CThostFtdcInputOrderField orderField = { 0 }; // 定义报单结构体

    // 设置报单字段
    strcpy(orderField.BrokerID, AppConfig::BrokerID.c_str());
    strcpy(orderField.InvestorID, AppConfig::InvestorID.c_str());
    strcpy(orderField.InstrumentID, instrumentID);
    strcpy(orderField.OrderRef, generateOrderRef(OrderRef_num++).c_str()); // 可以自定义
    orderField.Direction = direction; // 买卖方向
    orderField.CombOffsetFlag[0] = THOST_FTDC_OF_Open; // 开仓
    orderField.CombHedgeFlag[0] = THOST_FTDC_HF_Speculation; // 投机
    orderField.OrderPriceType = THOST_FTDC_OPT_LimitPrice; // 限价单
    orderField.LimitPrice = price; // 价格
    orderField.VolumeTotalOriginal = volume; // 交易量
    orderField.TimeCondition = THOST_FTDC_TC_GFD; // 当日有效
    orderField.VolumeCondition = THOST_FTDC_VC_AV; // 任意成交量
    orderField.ContingentCondition = THOST_FTDC_CC_Immediately; // 立即执行
    orderField.StopPrice = stopPrice; // 止损价
    strcpy(orderField.ExchangeID, getExchangeIdByInstrumentId(instrumentID).c_str()); // 交易所代码
    // 止盈价通常需要在条件单中设置，这里作为参数传递，但不会在报单中使用
    // 其他字段根据需要设置

    // 发送报单请求
    int iResult = api->ReqOrderInsert(&orderField, nRequestID++);
    if (iResult != 0) {
        // 处理错误情况
        printf("发送限价单失败，错误码：%d\n", iResult);
    }
}


void TraderClient::sendOrderByAnyPrice(const json& params) {
    CThostFtdcInputOrderField ord = { 0 };
    strcpy_s(ord.BrokerID, AppConfig::BrokerID.c_str());
    strcpy_s(ord.InvestorID, AppConfig::UserID.c_str());
    strcpy_s(ord.OrderRef, generateOrderRef(OrderRef_num++).c_str());

    // 从 params 中提取信息
    std::string instrumentID = params.at("instrumentId").get<std::string>();
    char direction = params.at("bs").get<std::string>()[0];
    char openOrClose = params.at("oc").get<std::string>()[0];

    strcpy_s(ord.InstrumentID, getInstrumentId(instrumentID).c_str());
    strcpy_s(ord.UserID, AppConfig::UserID.c_str());

    ord.OrderPriceType = THOST_FTDC_OPT_AnyPrice;
    ord.Direction = direction == 'b' ? THOST_FTDC_D_Buy : THOST_FTDC_D_Sell;
    ord.CombOffsetFlag[0] = openOrClose == 'o' ? THOST_FTDC_OF_Open : THOST_FTDC_OF_Close;
    ord.CombHedgeFlag[0] = THOST_FTDC_HF_Speculation;
    ord.LimitPrice = 0; // 市价单，无价格限制
    ord.VolumeTotalOriginal = params.at("vol").get<int>();
    ord.TimeCondition = THOST_FTDC_TC_IOC; // 立即完成，否则撤销
    ord.VolumeCondition = THOST_FTDC_VC_AV; // 任何数量
    ord.MinVolume = 1;
    ord.ContingentCondition = THOST_FTDC_CC_Immediately; // 立即
    ord.ForceCloseReason = THOST_FTDC_FCC_NotForceClose; // 非强平
    ord.IsAutoSuspend = 0;
    strcpy_s(ord.ExchangeID, getExchangeIdByInstrumentId(instrumentID).c_str());
    spdlog::info("\t经纪公司代码 = [{}]", ord.BrokerID);
    spdlog::info("\t投资者代码 = [{}]", ord.InvestorID);
    spdlog::info("\t报单引用 = [{}]", ord.OrderRef);
    spdlog::info("\t用户代码 = [{}]", ord.UserID);
    spdlog::info("\t合约代码 = [{}]", ord.InstrumentID);
    spdlog::info("\t买卖方向 = [{}]", ord.Direction == THOST_FTDC_D_Buy ? "买入" : "卖出");
    spdlog::info("\t开平标志 = [{}]", ord.CombOffsetFlag[0] == THOST_FTDC_OF_Open ? "开仓" : "平仓");
    spdlog::info("\t投机套保标志 = [{}]", ord.CombHedgeFlag[0] == THOST_FTDC_HF_Speculation ? "投机" : "其他");
    spdlog::info("\t价格条件 = [市价]");
    spdlog::info("\t数量 = [{}]", ord.VolumeTotalOriginal);
    spdlog::info("\t有效期类型 = [立即完成，否则撤销]");
    spdlog::info("\t成交量类型 = [任何数量]");
    spdlog::info("\t最小成交量 = [{}]", ord.MinVolume);
    spdlog::info("\t触发条件 = [立即]");
    spdlog::info("\t强平原因 = [非强平]");
    spdlog::info("\t交易所代码 = [{}]", ord.ExchangeID);
    spdlog::info("\t请求编号 = [{}]", nRequestID); // 使用nRequestID - 1，因为nRequestID已经自增
    int a = api->ReqOrderInsert(&ord, nRequestID++);
    spdlog::info("\t发送市价单请求 = [{}]\n", a);
}



void TraderClient::sendOrderTouch(const json& params) {
    CThostFtdcInputOrderField ord = { 0 };
    strcpy_s(ord.BrokerID, AppConfig::BrokerID.c_str());
    strcpy_s(ord.InvestorID, AppConfig::UserID.c_str());
    strcpy_s(ord.OrderRef, generateOrderRef(OrderRef_num++).c_str());

    // 从 params 中提取信息
    std::string instrumentID = params.at("instrumentId").get<std::string>();
    char direction = params.at("bs").get<std::string>()[0];
    char openOrClose = params.at("oc").get<std::string>()[0];
    double stopPrice = params.at("stopPrice").get<double>();
    double limitPrice = params.at("limitPrice").get<double>();

    strcpy_s(ord.InstrumentID, instrumentID.c_str());
    strcpy_s(ord.UserID, AppConfig::UserID.c_str());

    ord.OrderPriceType = THOST_FTDC_OPT_LimitPrice; // 限价单
    ord.Direction = direction == 'b' ? THOST_FTDC_D_Buy : THOST_FTDC_D_Sell;
    ord.CombOffsetFlag[0] = openOrClose == 'o' ? THOST_FTDC_OF_Open : THOST_FTDC_OF_Close;
    ord.CombHedgeFlag[0] = THOST_FTDC_HF_Speculation;
    ord.LimitPrice = limitPrice; // 限价
    ord.VolumeTotalOriginal = params.at("vol").get<int>();
    ord.TimeCondition = THOST_FTDC_TC_GFD; // 当日有效
    ord.VolumeCondition = THOST_FTDC_VC_AV; // 任何数量
    ord.MinVolume = 1;
    ord.ContingentCondition = THOST_FTDC_CC_Touch; // 触发条件
    ord.StopPrice = stopPrice; // 止损价
    ord.ForceCloseReason = THOST_FTDC_FCC_NotForceClose; // 非强平
    ord.IsAutoSuspend = 0;
    strcpy_s(ord.ExchangeID, getExchangeIdByInstrumentId(instrumentID).c_str());

    int a = api->ReqOrderInsert(&ord, nRequestID++);
    spdlog::info("\t发送止损限价单请求 = [{}]\n", a);
}

void TraderClient::OnRtnOrder(CThostFtdcOrderField* pOrder) {
    spdlog::info("<OnRtnOrder>");
    if (pOrder) {
        spdlog::debug("\t经纪公司代码 [{}]", pOrder->BrokerID);
        spdlog::debug("\t投资者代码 [{}]", pOrder->InvestorID);
        spdlog::debug("\t报单引用 [{}]", pOrder->OrderRef);
        spdlog::debug("\t用户代码 [{}]", pOrder->UserID);
        spdlog::debug("\t报单价格条件 [{}]", pOrder->OrderPriceType);
        spdlog::debug("\t买卖方向 [{}]", pOrder->Direction);
        spdlog::debug("\t组合开平标志 [{}]", pOrder->CombOffsetFlag);
        spdlog::debug("\t组合投机套保标志 [{}]", pOrder->CombHedgeFlag);
        spdlog::debug("\t价格 [{}]", pOrder->LimitPrice);
        spdlog::debug("\t数量 [{}]", pOrder->VolumeTotalOriginal);
        spdlog::debug("\t成交量类型 [{}]", pOrder->VolumeCondition);
        spdlog::debug("\t最小成交量 [{}]", pOrder->MinVolume);
        spdlog::debug("\t止损价 [{}]", pOrder->StopPrice);
        spdlog::debug("\t强平原因 [{}]", pOrder->ForceCloseReason);
        spdlog::debug("\t自动挂起标志 [{}]", pOrder->IsAutoSuspend);
        spdlog::debug("\t业务单元 [{}]", pOrder->BusinessUnit);
        spdlog::debug("\t请求编号 [{}]", pOrder->RequestID);
        spdlog::debug("\t本地报单编号 [{}]", pOrder->OrderLocalID);
        spdlog::debug("\t交易所代码 [{}]", pOrder->ExchangeID);
        spdlog::debug("\t会员代码 [{}]", pOrder->ParticipantID);
        spdlog::debug("\t客户代码 [{}]", pOrder->ClientID);
        spdlog::debug("\t交易所交易员代码 [{}]", pOrder->TraderID);
        spdlog::debug("\t安装编号 [{}]", pOrder->InstallID);
        spdlog::debug("\t报单提交状态 [{}]", pOrder->OrderSubmitStatus);
        spdlog::debug("\t报单提示序号 [{}]", pOrder->NotifySequence);
        spdlog::debug("\t交易日 [{}]", pOrder->TradingDay);
        spdlog::debug("\t结算编号 [{}]", pOrder->SettlementID);
        spdlog::debug("\t报单编号 [{}]", pOrder->OrderSysID);
        spdlog::debug("\t报单来源 [{}]", pOrder->OrderSource);
        spdlog::debug("\t报单状态 [{}]", pOrder->OrderStatus);
        spdlog::debug("\t报单类型 [{}]", pOrder->OrderType);
        spdlog::debug("\t今成交数量 [{}]", pOrder->VolumeTraded);
        spdlog::debug("\t剩余数量 [{}]", pOrder->VolumeTotal);
        spdlog::debug("\t报单日期 [{}]", pOrder->InsertDate);
        spdlog::debug("\t委托时间 [{}]", pOrder->InsertTime);
        spdlog::debug("\t激活时间 [{}]", pOrder->ActiveTime);
        spdlog::debug("\t挂起时间 [{}]", pOrder->SuspendTime);
        spdlog::debug("\t最后修改时间 [{}]", pOrder->UpdateTime);
        spdlog::debug("\t撤销时间 [{}]", pOrder->CancelTime);
        spdlog::debug("\t最后修改交易所交易员代码 [{}]", pOrder->ActiveTraderID);
        spdlog::debug("\t结算会员编号 [{}]", pOrder->ClearingPartID);
        spdlog::debug("\t序号 [{}]", pOrder->SequenceNo);
        spdlog::debug("\t前置编号 [{}]", pOrder->FrontID);
        spdlog::debug("\t会话编号 [{}]", pOrder->SessionID);
        spdlog::debug("\t用户强评标志 [{}]", pOrder->UserForceClose);
        spdlog::debug("\t操作用户代码 [{}]", pOrder->ActiveUserID);
        spdlog::debug("\t经纪公司报单编号 [{}]", pOrder->BrokerOrderSeq);
        spdlog::debug("\t相关报单 [{}]", pOrder->RelativeOrderSysID);
        spdlog::debug("\t郑商所成交数量 [{}]", pOrder->ZCETotalTradedVolume);
        spdlog::debug("\t互换单标志 [{}]", pOrder->IsSwapOrder);
        spdlog::debug("\t营业部编号 [{}]", pOrder->BranchID);
        spdlog::debug("\t投资单元代码 [{}]", pOrder->InvestUnitID);
        spdlog::debug("\t资金账号 [{}]", pOrder->AccountID);
        spdlog::debug("\t币种代码 [{}]", pOrder->CurrencyID);
        spdlog::debug("\t合约代码 [{}]", pOrder->InstrumentID);
        spdlog::debug("\t合约在交易所的代码 [{}]", pOrder->ExchangeInstID);
        spdlog::debug("\tIP地址 [{}]", pOrder->IPAddress);


        /*strcpy_s(pOrder.BrokerID, AppConfig::BrokerID.c_str());
        strcpy_s(pOrder.InvestorID, AppConfig::UserID.c_str());
        strcpy_s(pOrder.InstrumentID, instrumentID);
        strcpy_s(pOrder.UserID, AppConfig::UserID.c_str());*/


        /*strcpy_s(g_chOrderSysID, pOrder->OrderSysID);
        g_chFrontID = pOrder->FrontID;
        g_chSessionID = pOrder->SessionID;
        strcpy_s(g_chOrderRef, pOrder->OrderRef);
        strcpy_s(g_chExchangeID, pOrder->ExchangeID);*/

        // 处理报单状态
        // 使用 find 方法获取报单状态的中文描述
        auto it = orderStatusMap.find(pOrder->OrderStatus);
        std::string orderStatus = (it != orderStatusMap.end()) ? it->second : "未知状态";
        spdlog::info("报单状态: [{}]", orderStatus);
    }
    spdlog::info("</OnRtnOrder>");
}
void TraderClient::OnErrRtnOrderInsert(CThostFtdcInputOrderField* pInputOrder, CThostFtdcRspInfoField* pRspInfo) {
    spdlog::info("<OnErrRtnOrderInsert>");
    if (pInputOrder) {
        spdlog::debug("\t经纪公司代码 [{}]", pInputOrder->BrokerID);
        spdlog::debug("\t投资者代码 [{}]", GB2312ToUTF8(pInputOrder->InvestorID));
        spdlog::debug("\t保留的无效字段 [{}]", GB2312ToUTF8(pInputOrder->reserve1));
        spdlog::debug("\t报单引用 [{}]", GB2312ToUTF8(pInputOrder->OrderRef));
        spdlog::debug("\t用户代码 [{}]", GB2312ToUTF8(pInputOrder->UserID));
        spdlog::debug("\t报单价格条件 [{}]", pInputOrder->OrderPriceType);
        spdlog::debug("\t买卖方向 [{}]", pInputOrder->Direction);
        spdlog::debug("\t组合开平标志 [{}]", GB2312ToUTF8(pInputOrder->CombOffsetFlag));
        spdlog::debug("\t组合投机套保标志 [{}]", GB2312ToUTF8(pInputOrder->CombHedgeFlag));
        spdlog::debug("\t价格 [{}]", pInputOrder->LimitPrice);
        spdlog::debug("\t数量 [{}]", pInputOrder->VolumeTotalOriginal);
        spdlog::debug("\t有效期类型 [{}]", pInputOrder->TimeCondition);
        spdlog::debug("\tGTD日期 [{}]", GB2312ToUTF8(pInputOrder->GTDDate));
        spdlog::debug("\t成交量类型 [{}]", pInputOrder->VolumeCondition);
        spdlog::debug("\t最小成交量 [{}]", pInputOrder->MinVolume);
        spdlog::debug("\t触发条件 [{}]", pInputOrder->ContingentCondition);
        spdlog::debug("\t止损价 [{}]", pInputOrder->StopPrice);
        spdlog::debug("\t强平原因 [{}]", pInputOrder->ForceCloseReason);
        spdlog::debug("\t自动挂起标志 [{}]", pInputOrder->IsAutoSuspend);
        spdlog::debug("\t业务单元 [{}]", GB2312ToUTF8(pInputOrder->BusinessUnit));
        spdlog::debug("\t请求编号 [{}]", pInputOrder->RequestID);
        spdlog::debug("\t用户强评标志 [{}]", pInputOrder->UserForceClose);
        spdlog::debug("\t互换单标志 [{}]", pInputOrder->IsSwapOrder);
        spdlog::debug("\t交易所代码 [{}]", GB2312ToUTF8(pInputOrder->ExchangeID));
        spdlog::debug("\t投资单元代码 [{}]", GB2312ToUTF8(pInputOrder->InvestUnitID));
        spdlog::debug("\t资金账号 [{}]", GB2312ToUTF8(pInputOrder->AccountID));
        spdlog::debug("\t币种代码 [{}]", GB2312ToUTF8(pInputOrder->CurrencyID));
        spdlog::debug("\t交易编码 [{}]", GB2312ToUTF8(pInputOrder->ClientID));
        spdlog::debug("\t保留的无效字段 [{}]", GB2312ToUTF8(pInputOrder->reserve2));
        spdlog::debug("\tMac地址 [{}]", GB2312ToUTF8(pInputOrder->MacAddress));
        spdlog::debug("\t合约代码 [{}]", GB2312ToUTF8(pInputOrder->InstrumentID));
        spdlog::debug("\tIP地址 [{}]", GB2312ToUTF8(pInputOrder->IPAddress));
        json json_response;
        json_response["success"] = true;
        json_response["type"] = "errRtnOrderInsert";
        std::string jsonstr = json_response.dump();
        sendWebSocketMessage(jsonstr);
    }
    wsSendErrMsg(pRspInfo, "errRtnOrderInsert");
    spdlog::info("</OnErrRtnOrderInsert>");
}

// 方法：从Redis获取交易所品种信息并填充到json数组
void TraderClient::fetchInstrumentIdsFromRedis() {
    // 遍历交易所数组
    for (const auto& exchange : AppConfig::EXCHANGE_IDS) {
        // 从 Redis 读取该交易所的所有品种
        std::vector<std::string> instrumentIds = RedisUtil::listMsg("product::" + exchange);

        // 遍历品种数组，将交易所代码和品种代码组合后放入 JSON 数组
        for (const auto& instrumentId : instrumentIds) {
            instrumentIdArr.push_back(exchange + "." + instrumentId);
        }
    }
}
void TraderClient::qryInstrument(const json& params) {
    
    CThostFtdcQryInstrumentField q = { 0 };
    // 检查params中是否包含exchangeArr
    if (params.contains("exchangeArr") && !params["exchangeArr"].empty()) {
        for (const auto& exchange : params["exchangeArr"]) {
            strcpy_s(q.ExchangeID, exchange.get<std::string>().c_str());
            spdlog::info("<OnErrRtnOrderInsert>");
            int b = api->ReqQryInstrument(&q, nRequestID++);
            if (b != 0) {
                spdlog::error("ReqQryInstrument 网络问题: {}，交易所：{}", b, q.ExchangeID);
            }
            spdlog::info("</OnErrRtnOrderInsert>");
        }
    }
    else
    {
        for (const auto& exchange : AppConfig::EXCHANGE_IDS) {
            strcpy_s(q.ExchangeID, exchange.c_str());
            spdlog::info("<OnErrRtnOrderInsert>");
            int b = api->ReqQryInstrument(&q, nRequestID++);
            if (b != 0) {
                spdlog::error("ReqQryInstrument 网络问题: {}，交易所：{}", b, q.ExchangeID);
            }
            spdlog::info("</OnErrRtnOrderInsert>");
        }
    }
}

///请求查询合约响应
void TraderClient::OnRspQryInstrument(CThostFtdcInstrumentField* pInstrument, CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast)
{
    spdlog::info("<OnRspQryInstrument>");

    if (pInstrument) {

        // 获取当前时间
        std::time_t t = std::time(nullptr);
        std::tm* now = std::localtime(&t);

        // 将ExpireDate转换为tm结构
        std::tm expireDate = {};
        std::istringstream ss(pInstrument->ExpireDate);
        ss >> std::get_time(&expireDate, "%Y%m%d");

        // 比较ExpireDate和当前日期
        if (std::difftime(mktime(&expireDate), mktime(now)) > 0 and bIsLast) {
            // 如果ExpireDate大于当前日期，则合约未交割
            spdlog::debug("\t保留的无效字段1 [{}]", pInstrument->reserve1);
            spdlog::info("\t交易所代码 [{}]", pInstrument->ExchangeID);
            spdlog::debug("\t合约名称 [{}]", GB2312ToUTF8(pInstrument->InstrumentName));
            spdlog::debug("\t保留的无效字段2 [{}]", pInstrument->reserve2);
            spdlog::debug("\t保留的无效字段3 [{}]", pInstrument->reserve3);
            spdlog::debug("\t创建日 [{}]", pInstrument->CreateDate);
            spdlog::debug("\t上市日 [{}]", pInstrument->OpenDate);
            spdlog::debug("\t到期日 [{}]", pInstrument->ExpireDate);
            spdlog::debug("\t开始交割日 [{}]", pInstrument->StartDelivDate);
            spdlog::debug("\t结束交割日 [{}]", pInstrument->EndDelivDate);
            spdlog::debug("\t合约生命周期状态 [{}]", pInstrument->InstLifePhase);
            spdlog::debug("\t当前是否交易 [{}]", pInstrument->IsTrading);
            spdlog::debug("\t持仓类型 [{}]", pInstrument->PositionType);
            spdlog::debug("\t持仓日期类型 [{}]", pInstrument->PositionDateType);
            spdlog::debug("\t最大单边保证金算法 [{}]", pInstrument->MaxMarginSideAlgorithm);
            spdlog::debug("\t期权类型 [{}]", pInstrument->OptionsType);
            spdlog::debug("\t组合类型 [{}]", pInstrument->CombinationType);
            spdlog::debug("\t价格最小变动 [{}]", pInstrument->PriceTick);
            spdlog::debug("\t多头保证金率 [{}]", pInstrument->LongMarginRatio);
            spdlog::debug("\t空头保证金率 [{}]", pInstrument->ShortMarginRatio);
            spdlog::debug("\t执行价 [{}]", pInstrument->StrikePrice);
            spdlog::debug("\t合约基础商品乘数 [{}]", pInstrument->UnderlyingMultiple);
            spdlog::debug("\t合约代码 [{}]", pInstrument->InstrumentID);
            spdlog::debug("\t合约在交易所的代码 [{}]", pInstrument->ExchangeInstID);
            spdlog::debug("\t产品代码 [{}]", pInstrument->ProductID);
            spdlog::debug("\t基础商品代码 [{}]", pInstrument->UnderlyingInstrID);
            spdlog::debug("\t产品类型 [{}]", pInstrument->ProductClass);
            spdlog::debug("\t交割年份 [{}]", pInstrument->DeliveryYear);
            spdlog::debug("\t交割月 [{}]", pInstrument->DeliveryMonth);
            spdlog::debug("\t市价单最大下单量 [{}]", pInstrument->MaxMarketOrderVolume);
            spdlog::debug("\t市价单最小下单量 [{}]", pInstrument->MinMarketOrderVolume);
            spdlog::debug("\t限价单最大下单量 [{}]", pInstrument->MaxLimitOrderVolume);
            spdlog::debug("\t限价单最小下单量 [{}]", pInstrument->MinLimitOrderVolume);
            spdlog::debug("\t合约数量乘数 [{}]", pInstrument->VolumeMultiple);
            RedisUtil::pushMsg("product::"+std::string(pInstrument->ExchangeID), std::string(pInstrument->InstrumentID));
            //放redis
            instrumentIdArr.push_back(std::string(pInstrument->ExchangeID) + "." + std::string(pInstrument->InstrumentID));

        }
        }

    if (bIsLast) {
        // 打印json数组中的每一条记录
        //for (const auto& item : instrumentIdArr) {
        //    // 将json对象转换为字符串
        //    std::string instrumentIdStr = item.dump();
        //    // 使用spdlog打印
        //    spdlog::info("{}", instrumentIdStr);
        //}

        if (useWs && ws) {
            json json_response;
            json_response["success"] = true;
            json_response["data"] = instrumentIdArr;
            json_response["type"] = "qryInstrument";// 设置响应类型为 "rspQryInstrument"
            
            std::string jsonstr = json_response.dump();
            sendWebSocketMessage(jsonstr);
        }
       
    }

    spdlog::debug("\t请求编号 [{}]", nRequestID);
    spdlog::debug("\t是否最后一条 [{}]", bIsLast);
    wsSendErrMsg(pRspInfo, "qryInstrument");

    spdlog::info("</OnRspQryInstrument>");
    
}

void TraderClient::OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField* pSettlementInfoConfirm, CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast)
{
    spdlog::info("<OnRspSettlementInfoConfirm>");
    if (pSettlementInfoConfirm) {
        spdlog::info("\t经纪公司代码: [{}]", pSettlementInfoConfirm->BrokerID);
        spdlog::info("\t投资者代码: [{}]", pSettlementInfoConfirm->InvestorID);
        spdlog::info("\t确认日期: [{}]", pSettlementInfoConfirm->ConfirmDate);
        spdlog::info("\t确认时间: [{}]", pSettlementInfoConfirm->ConfirmTime);
        spdlog::info("\t账户ID: [{}]", pSettlementInfoConfirm->AccountID);
        spdlog::info("\t币种代码: [{}]", pSettlementInfoConfirm->CurrencyID);
        spdlog::info("\t结算编号: [{}]", pSettlementInfoConfirm->SettlementID);
    }
    wsSendErrMsg(pRspInfo, "settlementInfoConfirm");
    spdlog::info("\t请求ID: [{}]", nRequestID);
    spdlog::info("\t是否最后响应: [{}]", bIsLast ? "是" : "否");
    spdlog::info("</OnRspSettlementInfoConfirm>");
}

void TraderClient::confirmSettlementInfo() {
    CThostFtdcSettlementInfoConfirmField confirm = { 0 };
    strcpy_s(confirm.BrokerID, AppConfig::BrokerID.c_str());
    strcpy_s(confirm.InvestorID, AppConfig::UserID.c_str());

    int result = api->ReqSettlementInfoConfirm(&confirm, nRequestID++);
    spdlog::info("请求确认结算单......{}，序号=[{}]\n",
        result == 0 ? "发送成功" : "发送失败", result);
}






