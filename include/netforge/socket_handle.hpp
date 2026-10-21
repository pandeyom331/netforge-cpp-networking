
#pragma once

#include <unistd.h>
#include <utility>

namespace netforge {

class SocketHandle {
public:
    // Take ownership of an existing socket descriptor.
    explicit SocketHandle(int fd = -1) noexcept
        : fd_(fd)
    {
    }

    // Release the owned socket when this object is destroyed.
    ~SocketHandle()
    {
        reset();
    }

    // A socket must not have two independent owners.
    SocketHandle(const SocketHandle&) = delete;
    SocketHandle& operator=(const SocketHandle&) = delete;

    // Transfer ownership from another SocketHandle.
    SocketHandle(SocketHandle&& other) noexcept
        : fd_(std::exchange(other.fd_, -1))
    {
    }

    SocketHandle& operator=(SocketHandle&& other) noexcept
    {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }

        return *this;
    }

    // Access the descriptor for POSIX socket functions.
    int get() const noexcept
    {
        return fd_;
    }

    bool valid() const noexcept
    {
        return fd_ >= 0;
    }

    // Release ownership without closing the descriptor.
    int release() noexcept
    {
        return std::exchange(fd_, -1);
    }

    // Close the current descriptor and optionally take another.
    void reset(int new_fd = -1) noexcept
    {
        const int old_fd = std::exchange(fd_, new_fd);

        if (old_fd >= 0) {
            ::close(old_fd);
        }
    }

private:
    int fd_{-1};
};

} // namespace netforge
