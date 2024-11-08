#include "stdafx.h"
#include <hiredis/hiredis.h>
#include "RedisUtil.h"
//#include "main.h"
#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <thread>
#include <cstdlib> // 用于rand()
#include <uwebsockets/App.h>
#include "TraderClient.h"
#include "TraderClient.h"
#include "main.h"
//#include <uwebsockets>
//#include ""
using namespace std;

// 使用nlohmann的json库,并且取别名
using json = nlohmann::json;


std::shared_ptr<spdlog::sinks::basic_file_sink_mt> createLogFileSink() {
	// 获取当前时间
	auto now = std::chrono::system_clock::now();
	// 转换为 time_t 类型
	std::time_t now_c = std::chrono::system_clock::to_time_t(now);
	// 转换为 tm 结构体
	struct tm now_tm = *std::localtime(&now_c);

	// 创建日志文件名
	std::stringstream ss;
	ss << "logs/logfile"
		<< (1900 + now_tm.tm_year) << std::setw(2) << std::setfill('0') << (1 + now_tm.tm_mon) // 月份从0开始，所以加1
		<< std::setw(2) << std::setfill('0') << now_tm.tm_mday << "_"
		<< std::setw(2) << std::setfill('0') << now_tm.tm_hour << "_"
		<< std::setw(2) << std::setfill('0') << now_tm.tm_min << ".log";

	// 创建并返回文件日志接收器
	return std::make_shared<spdlog::sinks::basic_file_sink_mt>(ss.str(), true);
}

// 初始化 spdlog 的日志系统和线程池、配置类
//AppConfig g_AppConfig;
void init() {
	AppConfig::fromYaml("config.yaml"); //初始化配置
	spdlog::init_thread_pool(8192, 1); // 初始化线程池
	auto logLevel=static_cast<spdlog::level::level_enum>(AppConfig::LOG_LEVEL);
	// 创建控制台sink
	auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

	console_sink->set_level(logLevel);  // 设置控制台日志等级debug不显示
	console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [thread %t] [%^%l%$] %v"); // 设置日志格式

	// 创建文件sink
	auto file_sink = createLogFileSink();
	file_sink->set_level(logLevel);    // 设置文件日志等级
	//file_sink->set_pattern("[%T] [%l] %v");        // 设置日志格式
	file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e][thread %t] [%l] %v");        // 设置日志格式

	// 将多个sink组合到一个vector中
	std::vector<spdlog::sink_ptr> sinks{ console_sink, file_sink };

	// 创建一个多重sink的logger
	auto logger = std::make_shared<spdlog::logger>("multi_sink", sinks.begin(), sinks.end());
	spdlog::set_default_logger(logger);
	// 设置全局日志等级为debug，所有日志都会输出
	spdlog::set_level(spdlog::level::debug);
	// 开启日志刷新
	//spdlog::flush_on(spdlog::level::info);

	// 手动刷新日志，确保写入到文件
	spdlog::flush_every(std::chrono::seconds(1));  // 每秒刷新日志

		
	spdlog::info("变量和日志器初始化完毕");
	RedisUtil::initClients();
	
}



// 定义消息类型
enum class MessageType {
	Order,
	CancelOrder,
	ModifyStopLimit,
	QueryAccountInfo,
	Quote
};

// 全局变量
std::queue<std::string> messageQueue;
std::mutex queueMutex;
std::condition_variable queueCondition;

// WebSocket 服务器实例
std::shared_ptr<uWS::App> app;

// 后台线程函数
void backgroundThread() {
	while (true) {
		std::unique_lock<std::mutex> lock(queueMutex);
		queueCondition.wait(lock, [] { return !messageQueue.empty(); });
		std::string message = messageQueue.front();
		messageQueue.pop();
		lock.unlock();

		// 发送消息给所有客户端
		//app->broadcast(message);
	}
}

// Redis 客户端
cpp_redis::client redis_client;


int main()
{
	system("COLOR 0A");
	system("chcp 65001");//控制台utf-8输出
	init();
	RedisUtil::setStr("ctp_redis", "redis准备ok:必须发发发!");
	spdlog::info("redis状态：【{}】 ", RedisUtil::getStr("ctp_redis").value_or("redis不正常"));
	
	TraderClient traderClient;
	traderClient.init();
	traderClient.fetchInstrumentIdsFromRedis();
	traderClient.startWorkerThread();

	// 创建 uWS::App 实例
	uWS::App().ws<PerSocketData>("/ws", {
		.open = [&traderClient](auto* ws) {
			spdlog::info("建立连接");
			traderClient.onOpen(ws);
			traderClient.qryTradingAccount();
		},
		// WebSocket接收到消息时触发
		.message = [&traderClient](auto* ws, std::string_view message, uWS::OpCode opCode) {

			std::string message_str(message);
			if (message_str == "无我") {//心跳
				spdlog::info("收到心跳：{}", message_str);
			}
			else {
				spdlog::info("收到WebSocket消息：{}", message_str);
				traderClient.onMessage(ws, message, opCode);
			}
			
		},
		// WebSocket连接关闭时触发

		.close = [&traderClient](auto* ws, int code, std::string_view message) {
			spdlog::info("连接关闭");
			traderClient.onClose(ws,code,message);
		}
		})
		// 处理 GET 请求
			.get("/getInstruments", [&traderClient](auto* res, auto* req) {
			// 获取查询参数 'q'
			std::string query{ req->getParameter(0) };
			spdlog::info("收到GET请求：/getInstruments?q={}", query);
			// 过滤 instrumentIdArr 并限制结果为前10个
			nlohmann::json filteredArr;
			size_t count = 0; 
			for (const auto& instrument : traderClient.instrumentIdArr) {
				//if (instrument.contains("InstrumentID") && instrument["InstrumentID"].get<std::string>().find(query) != std::string::npos) {
				if (instrument.get<std::string>().find(query) != std::string::npos) {
					if (count < 20) {//最多返回20个

						//filteredArr.push_back(instrument);
						nlohmann::json item = {
							{"instrumentName", instrument}, // 假设 instrument 中有 "InstrumentName" 字段
							{"instrumentId", instrument}
						};
						filteredArr.push_back(item);
						++count;
					}
					else {
						break; // 找到10个后停止循环
					}
				}
			}
			res->writeHeader("Content-Type", "application/json");
			nlohmann::json response;
			response["success"] = "success";
			response["data"] = filteredArr;

			// 返回构造的 JSON 响应
			res->end(response.dump());
				})
		// 处理所有HTTP方法以设置CORS头部
			.any("/*", [](auto* res, auto* req) {
			// 设置 CORS 头部
			res->writeHeader("Access-Control-Allow-Origin", "*");
			res->writeHeader("Access-Control-Allow-Headers", "*");
			res->writeHeader("Access-Control-Allow-Methods", "*");

			// 如果是预检请求（OPTIONS），则直接返回
			if (req->getMethod() == "OPTIONS") {
				res->end(nullptr, 0);
				return;
			}
			res->end();
				})
		.listen(AppConfig::WEBSOCKET_PORT, [](auto* token) {
		if (token) {
			spdlog::info("服务器监听在端口 {}", AppConfig::WEBSOCKET_PORT);
		}
		else {
			spdlog::info("服务器监听在端口 {} 失败", AppConfig::WEBSOCKET_PORT);
			exit(1);
		}
		}).run();

	traderClient.stopWorkerThread(); // 停止后台线程
	spdlog::shutdown(); //等待日志线程关闭
	return 0;
}
