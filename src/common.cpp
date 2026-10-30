// =============================================================================
//  rtspmech - 错误码实现
// =============================================================================
#include <rtspmech/common.hpp>

namespace rtspmech {

namespace {

class RtspCategory final : public std::error_category {
public:
    [[nodiscard]] const char* name() const noexcept override { return "rtspmech"; }

    [[nodiscard]] std::string message(int code) const override
    {
        switch (static_cast<RtspErrc>(code)) {
        case RtspErrc::ok:                 return "ok";
        case RtspErrc::invalid_message:    return "invalid RTSP message";
        case RtspErrc::unsupported_version:return "unsupported RTSP version";
        case RtspErrc::too_large:          return "message exceeds size limit";
        case RtspErrc::not_attached:       return "session not attached";
        case RtspErrc::send_failed:        return "send failed";
        case RtspErrc::invalid_transport:  return "invalid Transport header";
        }
        return "unknown rtspmech error";
    }
};

} // namespace

const std::error_category& rtsp_category() noexcept
{
    static const RtspCategory kCategory;
    return kCategory;
}

std::error_code make_error_code(RtspErrc e) noexcept
{
    return {static_cast<int>(e), rtsp_category()};
}

} // namespace rtspmech
