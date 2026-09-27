#include "cfw/core/Error.h"

namespace cfw {

const char *errorCodeName(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::Unknown: return "Unknown";
    case ErrorCode::InvalidArgument: return "InvalidArgument";
    case ErrorCode::OutOfRange: return "OutOfRange";
    case ErrorCode::ParseError: return "ParseError";
    case ErrorCode::TypeMismatch: return "TypeMismatch";
    case ErrorCode::LimitExceeded: return "LimitExceeded";
    case ErrorCode::NotFound: return "NotFound";
    case ErrorCode::AlreadyExists: return "AlreadyExists";
    case ErrorCode::PermissionDenied: return "PermissionDenied";
    case ErrorCode::IoError: return "IoError";
    case ErrorCode::Unsupported: return "Unsupported";
    case ErrorCode::Cancelled: return "Cancelled";
    case ErrorCode::Timeout: return "Timeout";
    case ErrorCode::OutOfMemory: return "OutOfMemory";
    case ErrorCode::Corrupt: return "Corrupt";
    case ErrorCode::NetworkError: return "NetworkError";
    }
    return "Unknown";
}

String Error::describe() const {
    String text = errorCodeName(m_code);
    text += ": ";
    text += m_message;
    if (!m_context.empty()) {
        text += " (";
        for (std::size_t i = 0; i < m_context.size(); ++i) {
            if (i > 0) {
                text += ", ";
            }
            text += m_context[i].first;
            text += '=';
            text += m_context[i].second;
        }
        text += ')';
    }
    return text;
}

} // namespace cfw
