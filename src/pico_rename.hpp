// =============================================================================
//  rtspmech - 内部：vendored picohttpparser 的符号改名
// =============================================================================
//  picohttpparser 是 C 代码，函数是全局 C 符号；一旦编进本库（而不是单独出
//  libpicohttpparser.a），同进程里若还有 httpmech 也内嵌了一份，链接期就会
//  multiple definition。这里统一加 `rtspmech_` 前缀，各用各的名字。
//
//  用法：在**任何**包含 picohttpparser 的 .c/.h 之前先包含本文件。
// =============================================================================
#pragma once

#define phr_parse_request              rtspmech_phr_parse_request
#define phr_parse_response             rtspmech_phr_parse_response
#define phr_parse_headers              rtspmech_phr_parse_headers
#define phr_decode_chunked             rtspmech_phr_decode_chunked
#define phr_decode_chunked_is_in_data  rtspmech_phr_decode_chunked_is_in_data
