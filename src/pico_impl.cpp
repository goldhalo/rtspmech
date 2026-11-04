// =============================================================================
//  rtspmech - 把 vendored picohttpparser 编进本库
// =============================================================================
//  以前它是独立的 `libpicohttpparser.a`：使用方链接 rtspmech 时还得额外
//  `-lpicohttpparser`，安装目录里也会多出 `include/rtspmech/thirdparty/`。
//  但 pico 只是 parser 的实现细节（只有 src/parser.cpp 用），没理由让使用方买单。
//
//  现在把 .c 作为本库的一个编译单元，配合 pico_rename.hpp 的符号前缀：
//    · 发布物只有 librtspmech.a 一个库文件；
//    · 公开头目录里没有 picohttpparser.h；
//    · 与 httpmech 各自内嵌时不会撞符号（前缀不同）。
// =============================================================================
#include "pico_rename.hpp"

#include "../thirdparty/picohttpparser/picohttpparser.c"
