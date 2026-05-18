#include "stats.hpp"

#include <cstdint>
#include <cstdlib>
#include <cerrno>
#include <limits>
#include <mutex>

#include <fcntl.h>
#include <time.h>
#include <unistd.h>

namespace {

    constexpr std::uint32_t kMagic = 0x4653474c;
    constexpr std::uint32_t kVersion = 2;
    constexpr std::size_t kFrametimeCapacity = 256;

#pragma pack(push, 1)
    struct Snapshot {
        std::uint32_t magic{kMagic};
        std::uint32_t version{kVersion};
        std::uint64_t sequence{0};
        std::uint64_t totalPresents{0};
        std::uint64_t generatedPresents{0};
        std::uint64_t realPresents{0};
        std::uint64_t lastPresentNs{0};
        std::uint32_t multiplier{0};
        std::uint32_t reserved{0};
        std::uint64_t frametimeWriteIndex{0};
        std::uint32_t frametimeCount{0};
        std::uint32_t reserved2{0};
        std::uint32_t frametimeNs[kFrametimeCapacity]{};
    };
#pragma pack(pop)

    static_assert(sizeof(Snapshot) == 1096);

    std::mutex gMutex;
    Snapshot gSnapshot;
    int gFd{-2};

    void closeStatsFd() {
        if (gFd >= 0) {
            close(gFd);
            gFd = -1;
        }
    }

    std::uint64_t monotonicNs() {
        timespec ts{};
        if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
        return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL
            + static_cast<std::uint64_t>(ts.tv_nsec);
    }

    int statsFd() {
        if (gFd != -2) return gFd;

        const char* path = std::getenv("LSFG_STATS_PATH");
        if (path == nullptr || path[0] == '\0') {
            gFd = -1;
            return gFd;
        }

        gFd = open(path, O_CREAT | O_RDWR | O_TRUNC | O_CLOEXEC, 0666);
        if (gFd >= 0) {
            std::atexit(closeStatsFd);
            (void)ftruncate(gFd, static_cast<off_t>(sizeof(Snapshot)));
        }
        return gFd;
    }

    void writeSnapshot() {
        int fd = statsFd();
        if (fd < 0) return;

        const auto* data = reinterpret_cast<const std::uint8_t*>(&gSnapshot);
        std::size_t written = 0;
        while (written < sizeof(Snapshot)) {
            const ssize_t res = pwrite(fd, data + written, sizeof(Snapshot) - written,
                static_cast<off_t>(written));
            if (res < 0 && errno == EINTR) continue;
            if (res <= 0) break;
            written += static_cast<std::size_t>(res);
        }
    }

    void record(bool generated, std::size_t multiplier) {
        std::lock_guard lock(gMutex);

        gSnapshot.sequence++;
        writeSnapshot();

        const std::uint64_t now = monotonicNs();
        if (gSnapshot.lastPresentNs > 0 && now > gSnapshot.lastPresentNs) {
            const std::uint64_t delta = now - gSnapshot.lastPresentNs;
            const std::uint32_t clampedDelta = static_cast<std::uint32_t>(
                delta > std::numeric_limits<std::uint32_t>::max()
                    ? std::numeric_limits<std::uint32_t>::max()
                    : delta);
            gSnapshot.frametimeNs[gSnapshot.frametimeWriteIndex % kFrametimeCapacity] =
                clampedDelta;
            gSnapshot.frametimeWriteIndex++;
            if (gSnapshot.frametimeCount < kFrametimeCapacity) {
                gSnapshot.frametimeCount++;
            }
        }

        gSnapshot.totalPresents++;
        if (generated) {
            gSnapshot.generatedPresents++;
        } else {
            gSnapshot.realPresents++;
        }
        gSnapshot.lastPresentNs = now;
        gSnapshot.multiplier = static_cast<std::uint32_t>(multiplier);

        gSnapshot.sequence++;
        writeSnapshot();
    }

}

void Stats::recordGeneratedPresent(std::size_t multiplier) {
    record(true, multiplier);
}

void Stats::recordRealPresent(std::size_t multiplier) {
    record(false, multiplier);
}
