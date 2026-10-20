// SHA-256：只用于评测缓存指纹（SPEC §7.7）。
// 自己实现 100 行而不是引依赖，理由：指纹是评测正确性的一环（改二进制必须让缓存失效），
// 依赖 libcrypto 会把「构建可复现」绑到一个本项目并不需要的重库上。
#pragma once

#include <string>
#include <string_view>

namespace cue {

std::string sha256_hex(std::string_view data);

// 读文件内容算哈希（用于二进制 / suite 文件）；打不开或读失败返回空串
std::string sha256_file(const std::string& path);

}  // namespace cue
