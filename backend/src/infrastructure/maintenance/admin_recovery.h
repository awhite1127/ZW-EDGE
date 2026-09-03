// 板端 root-only 超级管理员密码恢复入口。
#pragma once

#include <string>

namespace edge_controller::maintenance {

// 交互式读取新密码并恢复已有超级管理员；返回适合作为进程退出码的数值。
int run_super_admin_recovery(const std::string& username);

}  // namespace edge_controller::maintenance
