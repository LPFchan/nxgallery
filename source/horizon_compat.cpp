// POSIX entry points required by the Switch OpenSSL/curl build but absent
// from libnx newlib. Horizon homebrew has one effective user and group.
#include <cerrno>
#include <cstddef>
#include <malloc.h>
#include <sys/types.h>

extern "C" {
#include <switch/kernel/random.h>

uid_t getuid() { return 0; }
uid_t geteuid() { return 0; }
gid_t getgid() { return 0; }
gid_t getegid() { return 0; }

int getentropy(void *buffer, size_t length) {
    if (length == 0) return 0;
    if (buffer == nullptr) { errno = EFAULT; return -1; }
    if (length > 256) { errno = EIO; return -1; }
    randomGet(buffer, length);
    return 0;
}

int posix_memalign(void **pointer, size_t alignment, size_t size) {
    if (pointer == nullptr) return EINVAL;
    if (alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0) return EINVAL;
    void *result = memalign(alignment, size == 0 ? alignment : size);
    if (result == nullptr) return ENOMEM;
    *pointer = result;
    return 0;
}

}  // extern "C"
