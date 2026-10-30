// =============================================================================
//  rtspmech - 汇总头（讨论稿）
// =============================================================================
//  一次性引入全部公共接口。业务代码通常只需要：
//      #include <rtspmech/rtspmech.hpp>
//
//  ★ 公共头只有 4 个
//      span.hpp           轻量 span
//      common.hpp         RtspErrc
//      rtsp_transport.hpp Transport 头解析（UDP / interleaved）
//      session.hpp        RtspSession / RtspHandler / Request/ResponseBuilder
//      rtspmech.hpp       本文件
//
//  ★ 定位回顾
//      网络 → netmech（TCP Conn）
//      解析 → picohttpparser（HTTP 风格的请求行/状态行/头部）
//      本库 → RTSP 独有的语义（CSeq / Session / Transport / $ 帧）+ 弱回调
//      不做 → 推流、取流、SDP 深度解析、认证握手
// =============================================================================
#pragma once

#include <rtspmech/span.hpp>
#include <rtspmech/common.hpp>
#include <rtspmech/rtsp_transport.hpp>
#include <rtspmech/session.hpp>
