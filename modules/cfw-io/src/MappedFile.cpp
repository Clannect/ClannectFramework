#include "cfw/io/MappedFile.h"

#include "PlatformFile.h"

namespace cfw {

MappedFile::MappedFile(MappedFile &&other) noexcept
    : m_data(std::exchange(other.m_data, nullptr)), m_size(std::exchange(other.m_size, 0)),
      m_fileHandle(std::exchange(other.m_fileHandle, nullptr)),
      m_mappingHandle(std::exchange(other.m_mappingHandle, nullptr)) {}

MappedFile &MappedFile::operator=(MappedFile &&other) noexcept {
    if (this != &other) {
        close();
        m_data = std::exchange(other.m_data, nullptr);
        m_size = std::exchange(other.m_size, 0);
        m_fileHandle = std::exchange(other.m_fileHandle, nullptr);
        m_mappingHandle = std::exchange(other.m_mappingHandle, nullptr);
    }
    return *this;
}

MappedFile::~MappedFile() { close(); }

Result<MappedFile> MappedFile::open(const Path &path, std::size_t maxBytes) {
    Result<detail::Mapping> mapping = detail::mapFile(path, maxBytes);
    if (!mapping) {
        return std::move(mapping).error();
    }
    MappedFile file;
    file.m_data = mapping.value().data;
    file.m_size = mapping.value().size;
    file.m_fileHandle = mapping.value().fileHandle;
    file.m_mappingHandle = mapping.value().mappingHandle;
    return file;
}

void MappedFile::close() noexcept {
    detail::Mapping mapping{m_data, m_size, m_fileHandle, m_mappingHandle};
    detail::unmapFile(mapping);
    m_data = nullptr;
    m_size = 0;
    m_fileHandle = nullptr;
    m_mappingHandle = nullptr;
}

} // namespace cfw
