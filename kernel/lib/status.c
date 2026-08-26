#include <jasos/status.h>

const char *status_name(status_t s)
{
    switch (s) {
    case STATUS_SUCCESS:                return "SUCCESS";
    case STATUS_ABANDONED:              return "ABANDONED";
    case STATUS_TIMEOUT:                return "TIMEOUT";
    case STATUS_PENDING:                return "PENDING";
    case STATUS_UNSUCCESSFUL:           return "UNSUCCESSFUL";
    case STATUS_NOT_IMPLEMENTED:        return "NOT_IMPLEMENTED";
    case STATUS_ACCESS_VIOLATION:       return "ACCESS_VIOLATION";
    case STATUS_INVALID_HANDLE:         return "INVALID_HANDLE";
    case STATUS_INVALID_PARAMETER:      return "INVALID_PARAMETER";
    case STATUS_NO_SUCH_FILE:           return "NO_SUCH_FILE";
    case STATUS_END_OF_FILE:            return "END_OF_FILE";
    case STATUS_NO_MEMORY:              return "NO_MEMORY";
    case STATUS_CONFLICTING_ADDRESSES:  return "CONFLICTING_ADDRESSES";
    case STATUS_ACCESS_DENIED:          return "ACCESS_DENIED";
    case STATUS_BUFFER_TOO_SMALL:       return "BUFFER_TOO_SMALL";
    case STATUS_OBJECT_TYPE_MISMATCH:   return "OBJECT_TYPE_MISMATCH";
    case STATUS_OBJECT_NAME_NOT_FOUND:  return "OBJECT_NAME_NOT_FOUND";
    case STATUS_OBJECT_NAME_COLLISION:  return "OBJECT_NAME_COLLISION";
    case STATUS_MUTANT_NOT_OWNED:       return "MUTANT_NOT_OWNED";
    case STATUS_INVALID_PAGE_PROTECTION:return "INVALID_PAGE_PROTECTION";
    case STATUS_DISK_FULL:              return "DISK_FULL";
    case STATUS_INSUFFICIENT_RESOURCES: return "INSUFFICIENT_RESOURCES";
    case STATUS_FILE_IS_A_DIRECTORY:    return "FILE_IS_A_DIRECTORY";
    case STATUS_NOT_A_DIRECTORY:        return "NOT_A_DIRECTORY";
    case STATUS_PROCESS_IS_TERMINATING: return "PROCESS_IS_TERMINATING";
    case STATUS_CANCELLED:              return "CANCELLED";
    case STATUS_NOT_SUPPORTED:          return "NOT_SUPPORTED";
    case STATUS_INVALID_IMAGE_FORMAT:   return "INVALID_IMAGE_FORMAT";
    default:                            return "STATUS_?";
    }
}
