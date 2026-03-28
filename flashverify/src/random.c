#include "random.h"
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>

void fill_random_buffer(uint8_t *buf, size_t len) {
    HCRYPTPROV hProv;
    CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT);
    CryptGenRandom(hProv, (DWORD)len, buf);
    CryptReleaseContext(hProv, 0);
}

#else
#include <sys/random.h>

void fill_random_buffer(uint8_t *buf, size_t len) {
    size_t done = 0;
    while (done < len) {
        ssize_t ret = getrandom(buf + done, len - done, 0);
        if (ret > 0) done += (size_t)ret;
    }
}

#endif
