// Host-only MouthLink transport test. Uses synthetic OpenCV-compatible Mat headers;
// it does not load PICO libraries, install hooks or access a camera.
#include "mouthlink/mouthlink.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using expression_high_rate::Mat;

[[noreturn]] void fail(const std::string& message) {
    std::fprintf(stderr, "FAIL: %s\n", message.c_str());
    std::exit(1);
}
void check(bool condition, const char* message) { if (!condition) fail(message); }
void log(const char* message) { std::fprintf(stderr, "%s\n", message); }

template<class T> void store(void* data, size_t offset, T value) {
    std::memcpy(static_cast<unsigned char*>(data) + offset, &value, sizeof(value));
}
template<class T> T load(const void* data, size_t offset) {
    T value;
    std::memcpy(&value, static_cast<const unsigned char*>(data) + offset, sizeof(value));
    return value;
}
uint32_t crc32(const unsigned char* data, size_t size) {
    uint32_t crc = 0xffffffffU;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = crc & 1 ? 0xedb88320U ^ (crc >> 1) : crc >> 1;
    }
    return crc ^ 0xffffffffU;
}

struct TestMat {
    Mat mat {};
    const unsigned char* data = nullptr;
    size_t step = 0;
    TestMat(int width, int height, int depth, const unsigned char* pixels, size_t stride)
        : data(pixels), step(stride) {
        store<uint32_t>(&mat, 0x00, 0x42ff0000U | uint32_t(depth));
        store<int32_t>(&mat, 0x04, 2);
        store<int32_t>(&mat, 0x08, height);
        store<int32_t>(&mat, 0x0c, width);
        store<const unsigned char*>(&mat, 0x10, data);
        store<const unsigned char*>(&mat, 0x20, data + (height - 1) * step + width * (depth ? 2 : 1));
        store<const size_t*>(&mat, 0x48, &step);
    }
};

int receiver = -1;
sockaddr_in sender {};
void send_control(const std::vector<unsigned char>& packet) {
    const ssize_t sent = sendto(receiver, packet.data(), packet.size(), 0,
                                reinterpret_cast<sockaddr*>(&sender), sizeof(sender));
    check(sent == ssize_t(packet.size()), "send control datagram");
}
std::vector<unsigned char> receive(int timeout_ms = 2000) {
    pollfd p {receiver, POLLIN, 0};
    const int ready = poll(&p, 1, timeout_ms);
    if (ready == 0) return {};
    check(ready > 0 && (p.revents & POLLIN), "poll datagram");
    unsigned char data[65536];
    const ssize_t size = recv(receiver, data, sizeof(data), 0);
    check(size >= 0, "receive datagram");
    return {data, data + size};
}
void expect_no_datagram() {
    const auto packet = receive(200);
    check(packet.empty(), "unexpected datagram");
}
void discover(bool active, uint16_t width, uint16_t height, uint8_t format) {
    send_control({0x06, 0x01});
    const auto hello = receive();
    check(hello.size() == 9 && hello[0] == 0x07 && hello[1] == 1, "HELLO shape");
    check(hello[8] & 1, "HELLO advertises STOCK");
    check(hello[2] == (active ? 1 : 0), "HELLO camera activity");
    check(hello[3] == format, "HELLO pixel format");
    check(load<uint16_t>(hello.data(), 4) == width && load<uint16_t>(hello.data(), 6) == height,
          "HELLO native dimensions");
}
void subscribe(uint16_t port, uint8_t fps = 0, uint16_t x = 0, uint16_t y = 0,
               uint16_t width = 0, uint16_t height = 0, int stock = -1) {
    // stock < 0: legacy 14-byte SUBSCRIBE; 0/1: 16-byte SUBSCRIBE with the flag byte.
    std::vector<unsigned char> packet(stock < 0 ? 14 : 16);
    if (stock > 0) packet[14] = 1;
    packet[0] = 0x02; packet[1] = 1;
    store<uint16_t>(packet.data(), 2, port);
    packet[4] = fps;
    store<uint16_t>(packet.data(), 6, x);
    store<uint16_t>(packet.data(), 8, y);
    store<uint16_t>(packet.data(), 10, width);
    store<uint16_t>(packet.data(), 12, height);
    send_control(packet);
    // UDP control and frame production are asynchronous; let the worker apply
    // the lease before injecting the next synthetic camera frame.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
}
struct Frame {
    uint16_t width = 0, height = 0, x = 0, y = 0;
    uint8_t format = 0;
    uint64_t source = 0;
    std::vector<unsigned char> payload;
};
// Counts single-fragment frames delivered for a synthetic camera ticking at
// period_ns (slightly faster than 90 Hz, as on the PICO 4 Pro mouth camera).
size_t count_rate(uint16_t reply_port, uint8_t fps, const TestMat& mat, uint64_t base,
                  uint64_t period_ns, size_t frames) {
    subscribe(reply_port, fps);
    size_t got = 0;
    for (size_t i = 0; i < frames; ++i) {
        mouthlink::offer(&mat.mat, base + i * period_ns);
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
        while (!receive(0).empty()) ++got;
    }
    while (!receive(100).empty()) ++got;
    return got;
}
Frame receive_frame(size_t expected_fragments) {
    Frame frame;
    size_t received = 0;
    for (size_t i = 0; i < expected_fragments; ++i) {
        const auto packet = receive();
        if (!(packet.size() >= 42 && packet[0] == 1 && packet[1] == 1)) {
            std::fprintf(stderr, "unexpected datagram size=%zu first=%02x %02x\n",
                         packet.size(), packet.empty() ? 0 : packet[0], packet.size() < 2 ? 0 : packet[1]);
            fail("FRAGMENT shape");
        }
        check(load<uint32_t>(packet.data(), 38) == crc32(packet.data(), 38), "header CRC32");
        const uint16_t index = load<uint16_t>(packet.data(), 28);
        const uint16_t count = load<uint16_t>(packet.data(), 30);
        const uint32_t offset = load<uint32_t>(packet.data(), 32);
        const uint16_t length = load<uint16_t>(packet.data(), 36);
        check(index == i && count == expected_fragments, "fragment indices");
        check(packet.size() == size_t(42 + length) && offset + length <= load<uint32_t>(packet.data(), 24),
              "fragment bounds");
        if (i == 0) {
            frame.width = load<uint16_t>(packet.data(), 16);
            frame.height = load<uint16_t>(packet.data(), 18);
            frame.x = load<uint16_t>(packet.data(), 20);
            frame.y = load<uint16_t>(packet.data(), 22);
            frame.format = packet[2];
            frame.source = load<uint64_t>(packet.data(), 8);
            frame.payload.resize(load<uint32_t>(packet.data(), 24));
        }
        std::memcpy(frame.payload.data() + offset, packet.data() + 42, length);
        received += length;
    }
    check(received == frame.payload.size(), "complete payload");
    return frame;
}
}

int main() {
    receiver = socket(AF_INET, SOCK_DGRAM, 0);
    check(receiver >= 0, "receiver socket");
    sockaddr_in local {};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local.sin_port = 0;
    check(bind(receiver, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0, "receiver bind");
    socklen_t local_size = sizeof(local);
    check(getsockname(receiver, reinterpret_cast<sockaddr*>(&local), &local_size) == 0, "receiver port");
    const uint16_t reply_port = ntohs(local.sin_port);
    sender = local;
    sender.sin_port = htons(19040);

    mouthlink::configure(true, 19040, log);
    mouthlink::start();
    discover(false, 0, 0, 0);

    const unsigned char gray8_pixels[] = {
        0,1,2,3,4,90,91,92,
        5,6,7,8,9,93,94,95,
        10,11,12,13,14,96,97,98,
    };
    TestMat gray8(5, 3, 0, gray8_pixels, 8);
    mouthlink::offer(&gray8.mat, 1000);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    discover(true, 5, 3, 1);

    unsigned char ping[16] {};
    ping[0] = 4; ping[1] = 1;
    store<uint32_t>(ping, 4, 0x12345678);
    store<uint64_t>(ping, 8, 0x1122334455667788ULL);
    send_control({ping, ping + 16});
    const auto pong = receive();
    check(pong.size() == 24 && pong[0] == 5 && pong[1] == 1, "PONG shape");
    check(load<uint32_t>(pong.data(), 4) == 0x12345678, "PONG sequence");
    check(load<uint64_t>(pong.data(), 8) == 0x1122334455667788ULL, "PONG PC timestamp");
    check(load<uint64_t>(pong.data(), 16) > 0, "PONG source monotonic timestamp");

    subscribe(reply_port, 0, 1, 1, 3, 2);
    mouthlink::offer(&gray8.mat, 1000);
    const auto roi = receive_frame(1);
    check(roi.width == 3 && roi.height == 2 && roi.x == 1 && roi.y == 1, "ROI header");
    check(roi.format == 1 && roi.source == 1000, "ROI format/source timestamp");
    const std::vector<unsigned char> expected_roi {6,7,8,11,12,13};
    check(roi.payload == expected_roi, "ROI payload and stride removal");

    mouthlink::offer(&gray8.mat, 1000); // Duplicate source must not be copied/sent.
    expect_no_datagram();

    subscribe(reply_port); // Full frame, native format, unlimited source rate.
    const unsigned char gray16_pixels[] = {
        0,0, 1,0, 2,0, 3,0, 99,99,99,99,
        4,0, 5,0, 6,0, 255,127, 99,99,99,99,
    };
    TestMat gray16(4, 2, 2, gray16_pixels, 12);
    mouthlink::offer(&gray16.mat, 2000);
    const auto full16 = receive_frame(1);
    check(full16.width == 4 && full16.height == 2 && full16.format == 2, "GRAY16 header");
    check(full16.payload.size() == 16 && full16.payload[14] == 255 && full16.payload[15] == 127,
          "GRAY16LE payload");

    std::vector<unsigned char> large_pixels(100 * 40);
    for (size_t i = 0; i < large_pixels.size(); ++i) large_pixels[i] = static_cast<unsigned char>(i * 31 + 7);
    TestMat large(100, 40, 0, large_pixels.data(), 100);
    mouthlink::offer(&large.mat, 3000);
    const auto fragmented = receive_frame(3);
    check(fragmented.payload == large_pixels, "1400-byte fragmentation/reassembly");

    // fps gate: camera period 11.111 ms (a hair under 1/90 s).
    const uint64_t cam = 11111000ULL;
    const size_t n = 90;
    size_t got90 = count_rate(reply_port, 90, gray8, 1000000000ULL, cam, n);
    size_t got0 = count_rate(reply_port, 0, gray8, 2000000000ULL, cam, n);
    size_t got60 = count_rate(reply_port, 60, gray8, 3000000000ULL, cam, n);
    size_t got45 = count_rate(reply_port, 45, gray8, 4000000000ULL, cam, n);
    size_t got30 = count_rate(reply_port, 30, gray8, 5000000000ULL, cam, n);
    std::fprintf(stderr, "fps gate: 90->%zu 0->%zu 60->%zu 45->%zu 30->%zu of %zu\n",
                 got90, got0, got60, got45, got30, n);
    check(got90 == n && got0 == n, "fps 90/0 must pass every camera frame");
    check(got60 >= 58 && got60 <= 62, "fps 60 must pass ~2/3 of 90 Hz frames");
    check(got45 >= 44 && got45 <= 46, "fps 45 must pass every other frame");
    check(got30 >= 29 && got30 <= 31, "fps 30 must pass every third frame");

    // STOCK: not sent without the flag, sent with it, CRC and fields intact.
    float raw[35], inst[33], fin[33];
    for (int i = 0; i < 35; ++i) raw[i] = 0.01f * float(i) - 0.1f;
    for (int i = 0; i < 33; ++i) { inst[i] = 0.02f * float(i); fin[i] = 0.5f + 0.01f * float(i); }
    subscribe(reply_port, 0, 0, 0, 0, 0, 0);
    mouthlink::offer_stock(777, raw, inst, fin, true, false, 1);
    expect_no_datagram();
    subscribe(reply_port, 0, 0, 0, 0, 0, 1);
    mouthlink::offer_stock(888, raw, inst, fin, true, true, -1);
    {
        const auto st = receive();
        check(st.size() == 424 && st[0] == 0x08 && st[1] == 1, "STOCK shape");
        check(load<uint32_t>(st.data(), 420) == crc32(st.data(), 420), "STOCK CRC32");
        check(st[2] == 3 && int8_t(st[3]) == -1, "STOCK flags/class");
        check(load<uint64_t>(st.data(), 8) == 888, "STOCK source timestamp");
        check(load<float>(st.data(), 16 + 4 * 34) == raw[34], "STOCK raw35");
        check(load<float>(st.data(), 156 + 4 * 32) == inst[32], "STOCK instant33");
        check(load<float>(st.data(), 288) == fin[0] && load<float>(st.data(), 288 + 4 * 32) == fin[32],
              "STOCK final33");
    }
    // A burst larger than the queue keeps the freshest records and never blocks.
    for (int i = 0; i < 100; ++i) mouthlink::offer_stock(1000 + i, raw, inst, fin, false, false, 0);
    {
        size_t got = 0; uint64_t last = 0;
        for (auto st = receive(300); !st.empty(); st = receive(300)) {
            if (st[0] == 0x08) { ++got; last = load<uint64_t>(st.data(), 8); }
        }
        check(got >= 32 && last == 1099, "STOCK burst keeps freshest");
    }
    subscribe(reply_port, 0, 0, 0, 0, 0, -1); // legacy SUBSCRIBE turns STOCK off
    mouthlink::offer_stock(5000, raw, inst, fin, true, false, 1);
    expect_no_datagram();

    send_control({0x03, 0x01});
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    mouthlink::offer(&gray8.mat, 4000);
    expect_no_datagram();

    close(receiver);
    std::puts("MouthLink host transport tests: OK");
    return 0;
}
