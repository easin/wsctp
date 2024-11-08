// stdafx.h : 标准系统包含文件的包含文件，
// 或是经常使用但不常更改的
// 特定于项目的包含文件
//

#pragma once
#ifndef STDAFX_H
#define STDAFX_H
#include "targetver.h"
#define _HAS_STD_BYTE 0 //或其他包含 std::byte 的头文件
#include <cstddef> // 或其他包含 std::byte 的头文件
#include <stdio.h>
#include <tchar.h>

// TODO:  在此处引用程序需要的其他头文件
// 声明并初始化全局配置实例
#include <iostream>
#include <string>
#include <yaml-cpp/yaml.h>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include "AppConfig.h"
// 使用 extern 声明全局配置实例
//extern AppConfig g_AppConfig;
//extern AppConfig g_AppConfig = AppConfig::fromYaml("config.yaml");

#endif // STDAFX_H