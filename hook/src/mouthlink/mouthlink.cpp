#include "mouthlink.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <new>
#include <pthread.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <atomic>

namespace mouthlink {
namespace {

constexpr uint16_t kDefaultControlPort = 9040;
constexpr size_t kHeaderSize = 42;
constexpr size_t kMaxFragment = 1400;
constexpr size_t kMaxFrameBytes = 4 * 1024 * 1024;
constexpr uint32_t kSubscribeLeaseMs = 3000;
constexpr uint8_t kVersion = 1;
constexpr uint8_t kFragment = 0x01, kSubscribe = 0x02, kUnsubscribe = 0x03;
constexpr uint8_t kPing = 0x04, kPong = 0x05, kDiscover = 0x06, kHello = 0x07;
constexpr uint8_t kGray8 = 0x01, kGray16Le = 0x02;

bool configured_enabled = false;
uint16_t configured_port = kDefaultControlPort;
LogFn configured_log = nullptr;

uint64_t monotonic_ns() {
    timespec ts {};
    return clock_gettime(CLOCK_MONOTONIC, &ts) == 0
        ? uint64_t(ts.tv_sec) * 1000000000ULL + uint64_t(ts.tv_nsec) : 0;
}
uint64_t monotonic_ms() { return monotonic_ns() / 1000000ULL; }

template<class T> T load(const void* object, size_t offset) {
    T value;
    std::memcpy(&value, static_cast<const unsigned char*>(object) + offset, sizeof(value));
    return value;
}
template<class T> void store(void* object, size_t offset, T value) {
    std::memcpy(static_cast<unsigned char*>(object) + offset, &value, sizeof(value));
}

uint32_t crc32(const unsigned char* data, size_t size) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t value = i;
            for (unsigned bit = 0; bit < 8; ++bit)
                value = value & 1 ? 0xedb88320U ^ (value >> 1) : value >> 1;
            table[i] = value;
        }
        ready = true;
    }
    uint32_t crc = 0xffffffffU;
    for (size_t i = 0; i < size; ++i)
        crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
    return crc ^ 0xffffffffU;
}

struct FrameView {
    const unsigned char* data = nullptr;
    uint16_t width = 0, height = 0;
    uint8_t format = 0;
    size_t step = 0, row_bytes = 0, payload_bytes = 0;
};

// PICO OS 5.13.7 uses the stock 64-bit OpenCV Mat ABI already reconstructed by
// PicoET: flags +0x00, rows +0x08, cols +0x0c, data +0x10, dataend +0x20,
// MatStep* +0x48. Payload is repacked row by row; wire data never includes stride.
bool inspect_mat(const expression_high_rate::Mat* mat, FrameView& view) {
    if (!mat) return false;
    const uint32_t flags = load<uint32_t>(mat, 0x00);
    const int dims = load<int32_t>(mat, 0x04);
    const int rows = load<int32_t>(mat, 0x08);
    const int cols = load<int32_t>(mat, 0x0c);
    const unsigned depth = flags & 7U;
    const unsigned channels = ((flags >> 3) & 0x1ffU) + 1U;
    if (dims != 2 || rows <= 0 || cols <= 0 || rows > 65535 || cols > 65535 || channels != 1)
        return false;
    if (depth != 0 && depth != 2) return false; // CV_8U / CV_16U, one channel.
    const size_t bytes_per_pixel = depth == 0 ? 1 : 2;
    const size_t row_bytes = size_t(cols) * bytes_per_pixel;
    const auto* steps = load<const size_t*>(mat, 0x48);
    const auto* data = load<const unsigned char*>(mat, 0x10);
    const auto* data_end = load<const unsigned char*>(mat, 0x20);
    if (!steps || !data) return false;
    const size_t step = *steps;
    if (step < row_bytes) return false;
    const uint64_t required = uint64_t(rows - 1) * step + row_bytes;
    const uint64_t payload = uint64_t(rows) * row_bytes;
    if (required > SIZE_MAX || payload > kMaxFrameBytes || payload > UINT32_MAX) return false;
    // Some external Mats have no dataend. When present, use it as an additional
    // bounds guard before touching camera memory.
    if (data_end) {
        const uintptr_t begin_address = reinterpret_cast<uintptr_t>(data);
        const uintptr_t end_address = reinterpret_cast<uintptr_t>(data_end);
        if (end_address < begin_address || end_address - begin_address < required) return false;
    }
    view.data = data;
    view.width = uint16_t(cols);
    view.height = uint16_t(rows);
    view.format = depth == 0 ? kGray8 : kGray16Le;
    view.step = step;
    view.row_bytes = row_bytes;
    view.payload_bytes = size_t(payload);
    return true;
}

class Sender {
    enum SlotState : unsigned char { Free, Ready, Sending };
    struct Slot {
        SlotState state = Free;
        uint16_t width = 0, height = 0;
        uint8_t format = 0;
        uint64_t source_ns = 0;
        size_t bytes = 0;
        unsigned char pixels[kMaxFrameBytes];
    };

    int fd_ = -1;
    uint16_t port_ = kDefaultControlPort;
    LogFn log_ = nullptr;
    pthread_t thread_ {};
    pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t changed_ = PTHREAD_COND_INITIALIZER;
    Slot slots_[2];
    int sending_slot_ = -1;

    std::atomic<bool> stream_requested {false};
    std::atomic<uint64_t> last_valid_ms {0};
    std::atomic<uint32_t> native_size {0}; // width in high 16, height in low 16.
    std::atomic<uint8_t> native_format {0};
    std::atomic<uint64_t> last_copied_source {0};
    std::atomic<uint64_t> copied_frames {0}, copy_drops {0};

    sockaddr_in subscriber_ {};
    bool subscribed_ = false;
    uint64_t lease_until_ms_ = 0;
    uint8_t target_fps_ = 0, requested_format_ = 0;
    uint16_t roi_x_ = 0, roi_y_ = 0, roi_w_ = 0, roi_h_ = 0;
    uint64_t next_source_ns_ = 0, last_sent_source_ns_ = 0;
    bool have_sent_source_ = false;
    uint32_t next_frame_id_ = 0;
    uint64_t sent_frames_ = 0, sent_fragments_ = 0, send_drops_ = 0, send_errors_ = 0;
    uint64_t last_stats_ms_ = 0;
    uint16_t logged_width_ = 0, logged_height_ = 0;
    uint8_t logged_format_ = 0;

    static void* entry(void* self) {
        static_cast<Sender*>(self)->run();
        return nullptr;
    }
    void log(const char* fmt, ...) const {
        if (!log_) return;
        char message[512];
        va_list args;
        va_start(args, fmt);
        vsnprintf(message, sizeof(message), fmt, args);
        va_end(args);
        log_(message);
    }
    static void add_ms(timespec& ts, uint64_t ms) {
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += time_t(ms / 1000);
        ts.tv_nsec += long(ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ++ts.tv_sec; ts.tv_nsec -= 1000000000L; }
    }
    bool send(const void* data, size_t size, const sockaddr_in& to) {
        const ssize_t result = sendto(fd_, data, size, MSG_DONTWAIT | MSG_NOSIGNAL,
                                      reinterpret_cast<const sockaddr*>(&to), sizeof(to));
        if (result == ssize_t(size)) return true;
        ++send_errors_;
        return false;
    }
    void unsubscribe(const char* reason) {
        if (!subscribed_) return;
        subscribed_ = false;
        stream_requested.store(false, std::memory_order_release);
        next_source_ns_ = 0;
        log("MouthLink: stream stopped (%s)", reason);
    }
    void expire(uint64_t now_ms) {
        if (subscribed_ && now_ms > lease_until_ms_) unsubscribe("subscription lease expired");
    }
    void handle_subscribe(const unsigned char* data, size_t size, const sockaddr_in& from) {
        if (size < 14) return;
        const uint16_t reply_port = load<uint16_t>(data, 2);
        if (!reply_port) return;
        subscriber_ = {};
        subscriber_.sin_family = AF_INET;
        subscriber_.sin_addr = from.sin_addr;
        subscriber_.sin_port = htons(reply_port);
        target_fps_ = data[4] > 90 ? 90 : data[4];
        requested_format_ = data[5];
        roi_x_ = load<uint16_t>(data, 6);
        roi_y_ = load<uint16_t>(data, 8);
        roi_w_ = load<uint16_t>(data, 10);
        roi_h_ = load<uint16_t>(data, 12);
        lease_until_ms_ = monotonic_ms() + kSubscribeLeaseMs;
        next_source_ns_ = 0;
        const bool was_subscribed = subscribed_;
        subscribed_ = true;
        stream_requested.store(true, std::memory_order_release);
        if (!was_subscribed) {
            char address[INET_ADDRSTRLEN] = "?";
            inet_ntop(AF_INET, &from.sin_addr, address, sizeof(address));
            log("MouthLink: SUBSCRIBE %s:%u fps=%u fmt_req=0x%02x ROI=%u,%u %ux%u",
                address, unsigned(reply_port), unsigned(target_fps_), unsigned(requested_format_),
                unsigned(roi_x_), unsigned(roi_y_), unsigned(roi_w_), unsigned(roi_h_));
        }
    }
    void handle_control() {
        for (;;) {
            unsigned char data[2048];
            sockaddr_in from {};
            socklen_t from_size = sizeof(from);
            const ssize_t size = recvfrom(fd_, data, sizeof(data), MSG_DONTWAIT,
                                          reinterpret_cast<sockaddr*>(&from), &from_size);
            if (size < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                    log("MouthLink: recvfrom failed errno=%d", errno);
                return;
            }
            if (size < 2 || from_size != sizeof(from)) continue;
            switch (data[0]) {
            case kSubscribe:
                handle_subscribe(data, size_t(size), from);
                break;
            case kUnsubscribe:
                unsubscribe("UNSUBSCRIBE");
                break;
            case kPing:
                if (size >= 16) {
                    unsigned char pong[24] {};
                    pong[0] = kPong;
                    pong[1] = kVersion;
                    store<uint32_t>(pong, 4, load<uint32_t>(data, 4));
                    store<uint64_t>(pong, 8, load<uint64_t>(data, 8));
                    store<uint64_t>(pong, 16, monotonic_ns());
                    send(pong, sizeof(pong), from);
                }
                break;
            case kDiscover:
            {
                unsigned char hello[8] {};
                hello[0] = kHello;
                hello[1] = kVersion;
                const uint64_t last = last_valid_ms.load(std::memory_order_acquire);
                const uint64_t now = monotonic_ms();
                hello[2] = last && now >= last && now - last <= 250 ? 1 : 0;
                hello[3] = native_format.load(std::memory_order_acquire);
                const uint32_t dimensions = native_size.load(std::memory_order_acquire);
                store<uint16_t>(hello, 4, uint16_t(dimensions >> 16));
                store<uint16_t>(hello, 6, uint16_t(dimensions));
                send(hello, sizeof(hello), from);
                break;
            }
            default:
                break;
            }
        }
    }
    int take_ready() {
        pthread_mutex_lock(&mutex_);
        timespec wake;
        add_ms(wake, 5);
        int ready = -1;
        for (unsigned i = 0; i < 2; ++i) if (slots_[i].state == Ready) ready = int(i);
        if (ready < 0) {
            pthread_cond_timedwait(&changed_, &mutex_, &wake);
            for (unsigned i = 0; i < 2; ++i) if (slots_[i].state == Ready) ready = int(i);
        }
        if (ready >= 0) {
            slots_[ready].state = Sending;
            sending_slot_ = ready;
        }
        pthread_mutex_unlock(&mutex_);
        return ready;
    }
    void finish_send(int slot) {
        pthread_mutex_lock(&mutex_);
        slots_[slot].state = Free;
        sending_slot_ = -1;
        pthread_mutex_unlock(&mutex_);
    }
    void maybe_log_format(const Slot& slot) {
        if (slot.width == logged_width_ && slot.height == logged_height_ && slot.format == logged_format_)
            return;
        logged_width_ = slot.width;
        logged_height_ = slot.height;
        logged_format_ = slot.format;
        log("MouthLink: mouth Mat %ux%u %s packed=%u source_ns=%llu",
            unsigned(slot.width), unsigned(slot.height), slot.format == kGray8 ? "GRAY8" : "GRAY16LE",
            unsigned(slot.bytes), (unsigned long long)slot.source_ns);
    }
    void send_frame(const Slot& slot) {
        maybe_log_format(slot);
        if (!subscribed_) return;
        if (requested_format_ && requested_format_ != slot.format) {
            // No lossy or bit-depth conversion is hidden behind the request flag.
            // Native format is advertised in every header and in HELLO.
        }
        if (have_sent_source_ && slot.source_ns && slot.source_ns == last_sent_source_ns_) return;
        if (have_sent_source_ && slot.source_ns < last_sent_source_ns_) {
            // A recreated FaceTrackor owner may restart its source clock without
            // restarting the service. Do not let the old FPS deadline mute it.
            next_source_ns_ = 0;
        }
        // fps >= 90 is the camera ceiling: no gating, every distinct frame is sent.
        // Below that, keep a fixed source-clock grid with a quarter-period slack.
        // Without slack a camera ticking a few ns faster than 1/fps misses every
        // deadline by a hair and the effective rate halves (90 -> 45, 60 -> 45).
        if (target_fps_ && target_fps_ < 90 && slot.source_ns) {
            const uint64_t period = 1000000000ULL / target_fps_;
            const uint64_t slack = period / 4;
            if (next_source_ns_ && slot.source_ns + slack < next_source_ns_) return;
            // Advance the grid; resynchronise after a gap (headset off, pause)
            // instead of bursting to catch up.
            if (!next_source_ns_ || slot.source_ns >= next_source_ns_ + period)
                next_source_ns_ = slot.source_ns + period;
            else
                next_source_ns_ += period;
        }
        uint16_t x = 0, y = 0, width = slot.width, height = slot.height;
        if (roi_w_ && roi_h_ && roi_x_ < slot.width && roi_y_ < slot.height &&
            uint32_t(roi_x_) + roi_w_ <= slot.width && uint32_t(roi_y_) + roi_h_ <= slot.height) {
            x = roi_x_; y = roi_y_; width = roi_w_; height = roi_h_;
        }
        const size_t bpp = slot.format == kGray8 ? 1 : 2;
        const size_t row_bytes = size_t(slot.width) * bpp;
        const size_t out_row_bytes = size_t(width) * bpp;
        const size_t payload_bytes = out_row_bytes * height;
        const size_t fragment_count = (payload_bytes + kMaxFragment - 1) / kMaxFragment;
        if (!payload_bytes || fragment_count > 65535) { ++send_drops_; return; }
        unsigned char datagram[kHeaderSize + kMaxFragment];
        const uint32_t frame_id = next_frame_id_++;
        for (size_t index = 0, offset = 0; index < fragment_count; ++index, offset += kMaxFragment) {
            const size_t length = payload_bytes - offset < kMaxFragment ? payload_bytes - offset : kMaxFragment;
            std::memset(datagram, 0, kHeaderSize);
            datagram[0] = kFragment;
            datagram[1] = kVersion;
            datagram[2] = slot.format;
            datagram[3] = 0; // Compression.None
            store<uint32_t>(datagram, 4, frame_id);
            store<uint64_t>(datagram, 8, slot.source_ns);
            store<uint16_t>(datagram, 16, width);
            store<uint16_t>(datagram, 18, height);
            store<uint16_t>(datagram, 20, x);
            store<uint16_t>(datagram, 22, y);
            store<uint32_t>(datagram, 24, uint32_t(payload_bytes));
            store<uint16_t>(datagram, 28, uint16_t(index));
            store<uint16_t>(datagram, 30, uint16_t(fragment_count));
            store<uint32_t>(datagram, 32, uint32_t(offset));
            store<uint16_t>(datagram, 36, uint16_t(length));
            store<uint32_t>(datagram, 38, crc32(datagram, 38));
            // A fragment can cross a row boundary. Repack the selected ROI without
            // allocating a second full-frame buffer in the sender thread.
            size_t done = 0, logical = offset;
            while (done < length) {
                const size_t row = logical / out_row_bytes;
                const size_t column = logical % out_row_bytes;
                const size_t chunk = out_row_bytes - column < length - done
                    ? out_row_bytes - column : length - done;
                const size_t actual_row = size_t(y) + row;
                const size_t source_offset = actual_row * row_bytes + (size_t(x) * bpp) + column;
                std::memcpy(datagram + kHeaderSize + done, slot.pixels + source_offset, chunk);
                done += chunk;
                logical += chunk;
            }
            if (!send(datagram, kHeaderSize + length, subscriber_)) {
                ++send_drops_;
                return; // A fresh frame is more useful than a backed-up old one.
            }
            ++sent_fragments_;
        }
        ++sent_frames_;
        last_sent_source_ns_ = slot.source_ns;
        have_sent_source_ = true;
    }
    void stats(uint64_t now_ms) {
        if (!last_stats_ms_) { last_stats_ms_ = now_ms; return; }
        if (now_ms - last_stats_ms_ < 5000) return;
        if (subscribed_ || send_errors_ || copy_drops)
            log("MouthLink: copied=%llu copy_drops=%llu sent=%llu fragments=%llu send_drops=%llu errors=%llu",
                (unsigned long long)copied_frames.load(), (unsigned long long)copy_drops.load(),
                (unsigned long long)sent_frames_, (unsigned long long)sent_fragments_,
                (unsigned long long)send_drops_, (unsigned long long)send_errors_);
        last_stats_ms_ = now_ms;
    }

public:
    Sender(uint16_t port, LogFn log) : port_(port), log_(log) {}
    bool start() {
        fd_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fd_ < 0) { log("MouthLink: socket failed errno=%d", errno); return false; }
        const int flags = fcntl(fd_, F_GETFL, 0);
        if (flags >= 0) fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
        int buffer_size = 4 * 1024 * 1024;
        setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size));
        buffer_size = 1024 * 1024;
        setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size));
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(port_);
        if (bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            log("MouthLink: bind UDP %u failed errno=%d", unsigned(port_), errno);
            close(fd_); fd_ = -1;
            return false;
        }
        if (pthread_create(&thread_, nullptr, entry, this) != 0) {
            log("MouthLink: sender thread creation failed");
            close(fd_); fd_ = -1;
            return false;
        }
        pthread_detach(thread_);
        log("MouthLink: listening on UDP %u; raw streaming waits for SUBSCRIBE", unsigned(port_));
        return true;
    }
    void offer(const expression_high_rate::Mat* mat, uint64_t source_ns) {
        FrameView view;
        if (!inspect_mat(mat, view)) return;
        last_valid_ms.store(monotonic_ms(), std::memory_order_release);
        native_size.store((uint32_t(view.width) << 16) | view.height, std::memory_order_release);
        native_format.store(view.format, std::memory_order_release);
        if (!stream_requested.load(std::memory_order_acquire)) return;
        if (source_ns && last_copied_source.load(std::memory_order_acquire) == source_ns) return;

        // Never wait for the sender while holding the tracking thread. Overwrite
        // a not-yet-sent frame (latest wins), or use a free slot. The slot being
        // fragmented on the worker is never touched.
        if (pthread_mutex_trylock(&mutex_) != 0) {
            ++copy_drops;
            return;
        }
        int selected = -1;
        for (unsigned i = 0; i < 2; ++i) if (slots_[i].state == Ready) selected = int(i);
        if (selected < 0)
            for (unsigned i = 0; i < 2; ++i) if (slots_[i].state == Free) { selected = int(i); break; }
        if (selected < 0) {
            pthread_mutex_unlock(&mutex_);
            ++copy_drops;
            return;
        }
        Slot& slot = slots_[selected];
        slot.width = view.width;
        slot.height = view.height;
        slot.format = view.format;
        slot.source_ns = source_ns;
        slot.bytes = view.payload_bytes;
        if (view.step == view.row_bytes) {
            std::memcpy(slot.pixels, view.data, view.payload_bytes);
        } else {
            for (size_t row = 0; row < view.height; ++row)
                std::memcpy(slot.pixels + row * view.row_bytes,
                            view.data + row * view.step, view.row_bytes);
        }
        slot.state = Ready;
        ++copied_frames;
        last_copied_source.store(source_ns, std::memory_order_release);
        pthread_cond_signal(&changed_);
        pthread_mutex_unlock(&mutex_);
    }
    void run() {
        for (;;) {
            handle_control();
            expire(monotonic_ms());
            const int slot = take_ready();
            if (slot >= 0) {
                send_frame(slots_[slot]);
                finish_send(slot);
            }
            stats(monotonic_ms());
        }
    }
};

std::atomic<Sender*> global_sender {nullptr};

} // namespace

void configure(bool enabled, uint16_t control_port, LogFn log) noexcept {
    configured_enabled = enabled;
    configured_port = control_port ? control_port : kDefaultControlPort;
    configured_log = log;
}
void start() noexcept {
    if (!configured_enabled || global_sender.load(std::memory_order_acquire)) return;
    auto* sender = new (std::nothrow) Sender(configured_port, configured_log);
    if (!sender) {
        if (configured_log) configured_log("MouthLink: cannot allocate sender; disabled");
        return;
    }
    if (!sender->start()) {
        delete sender;
        return;
    }
    // Publish only a fully running sender. A hook can arrive as soon as the
    // expression plugin is installed, including while bind/thread setup fails.
    global_sender.store(sender, std::memory_order_release);
}
void offer(const expression_high_rate::Mat* mouth, uint64_t source_ns) noexcept {
    auto* sender = global_sender.load(std::memory_order_acquire);
    if (!sender) return;
    try { sender->offer(mouth, source_ns); }
    catch (...) { /* Optional streaming must never destabilize expression tracking. */ }
}

} // namespace mouthlink
