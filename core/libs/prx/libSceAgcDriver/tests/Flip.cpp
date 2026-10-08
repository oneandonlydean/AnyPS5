#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libc/include/Shutdown.hpp"
#include "prx/libSceAgcDriver/Execution/include/VideoOutput.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libSceAgcDriver/Submit/include/Acb.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void check(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

template<typename TAction>
std::string expectFailure(TAction action) {
    try { action(); }
    catch (const std::runtime_error& error) { return error.what(); }
    throw std::runtime_error("expected an exception");
}

struct State {
    std::mutex mutex;
    std::condition_variable changed;
    bool block = false;
    bool entered = false;
    bool fail = false;
    bool checkSelfWait = false;
    std::atomic<int> alive = 0;
    std::atomic<int> ready = 0;
    std::atomic<int> failed = 0;
    AgcDriver::FlipInfo last{};
};

class FlipRelease final {
public:
    explicit FlipRelease(std::shared_ptr<State> state) : state(std::move(state)) {}
    ~FlipRelease() { Release(); }
    void Release() {
        {
            std::lock_guard lock(state->mutex);
            state->block = false;
        }
        state->changed.notify_all();
    }
private:
    std::shared_ptr<State> state;
};

class Request final : public AgcDriver::IFlipRequest {
public:
    explicit Request(std::shared_ptr<State> value) : state(std::move(value)) { ++state->alive; }
    ~Request() override { --state->alive; }
    void GpuReady(const std::shared_ptr<AgcDriver::FrameTiming>&) override {
        std::unique_lock lock(state->mutex);
        if (state->checkSelfWait) {
            check(expectFailure([] { AgcDriverSuspendPoint_nid_postfix(); }).find("itself") != std::string::npos, "self suspend was not rejected");
            check(expectFailure([] { AgcDriverWaitIdle_nid_postfix(); }).find("itself") != std::string::npos, "self wait was not rejected");
        }
        state->entered = true;
        state->changed.notify_all();
        state->changed.wait(lock, [&] { return !state->block; });
        if (state->fail) throw std::runtime_error("intentional flip failure");
        ++state->ready;
    }
    void Fail(std::exception_ptr error) noexcept override {
        if (!error) std::terminate();
        ++state->failed;
    }
private:
    std::shared_ptr<State> state;
};

class Output final : public AgcDriver::IVideoOutput {
public:
    void Fail(std::exception_ptr error) noexcept override { if (!error) std::terminate(); }
    std::shared_ptr<State> state = std::make_shared<State>();
    std::shared_ptr<AgcDriver::IFlipRequest> Reserve(const AgcDriver::FlipInfo& info) override {
        std::lock_guard lock(state->mutex);
        state->last = info;
        return std::make_shared<Request>(state);
    }
};

void submitFlip(std::uint32_t handle = 7) {
    std::array<std::uint32_t, 6> words{0xc004105c, handle, 0xfffffffeu, 1, 0x76543211u, 0xfedcba98u};
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    check(sceAgcDriverSubmitDcb(&packet) == 0, "flip submission failed");
}

void testFlipAndBoundary() {
    auto output = std::make_shared<Output>();
    AgcDriverRegisterVideoOutput_nid_postfix(7, output);
    expectFailure([&] { AgcDriverRegisterVideoOutput_nid_postfix(7, output); });
    std::array<std::uint32_t, 6> words{0xc004105c, 7, 0xfffffffeu, 1, 0, 0};
    Packet packet{words.data(), 6, 0, {}};
    expectFailure([&] { sceAgcDriverSubmitAcb(0x20, &packet); });
    words[0] = 0xc004105d;
    expectFailure([&] { sceAgcDriverSubmitDcb(&packet); });
    words[0] = 0xc004105c;
    packet.dw_num = 5;
    expectFailure([&] { sceAgcDriverSubmitDcb(&packet); });
    expectFailure([] { submitFlip(8); });
    std::array<std::uint32_t, 12> rollback{0xc004105c, 7, 0, 1, 0, 0, 0xc004105c, 8, 0, 1, 0, 0};
    Packet rejected{rollback.data(), 12, 0, {}};
    expectFailure([&] { sceAgcDriverSubmitDcb(&rejected); });
    check(output->state->alive == 0 && output->state->ready == 0, "rejected submission retained or executed a reservation");
    alignas(4) static std::uint32_t condition = 1;
    const auto conditionAddress = reinterpret_cast<std::uintptr_t>(&condition);
    std::array<std::uint32_t, 11> guarded{0xc0032200, static_cast<std::uint32_t>(conditionAddress), static_cast<std::uint32_t>(conditionAddress >> 32u), 0, 6, 0xc004105c, 7, 0, 1, 0, 0};
    Packet conditionalFlip{guarded.data(), 11, 0, {}};
    check(expectFailure([&] { sceAgcDriverSubmitDcb(&conditionalFlip); }).find("a flip inside a conditional execution range") != std::string::npos, "a flip inside a COND_EXEC range was not rejected");
    check(output->state->alive == 0 && output->state->ready == 0, "a rejected conditional flip retained or executed a reservation");
    {
        std::lock_guard lock(output->state->mutex);
        output->state->block = true;
        output->state->checkSelfWait = true;
    }
    std::future<void> boundary;
    FlipRelease release(output->state);
    submitFlip();
    {
        std::unique_lock lock(output->state->mutex);
        check(output->state->changed.wait_for(lock, std::chrono::seconds(5), [&] { return output->state->entered; }), "worker did not reach flip");
        check(output->state->last.argument == -0x123456789abcdefLL && output->state->last.index == -2, "decoded flip arguments changed");
    }
    boundary = std::async(std::launch::async, [] { AgcDriverSuspendPoint_nid_postfix(); });
    check(boundary.wait_for(std::chrono::seconds(5)) == std::future_status::ready, "suspend blocked on preceding work");
    boundary.get();
    check(output->state->ready == 0, "blocked flip completed before release");
    AgcDriverUnregisterVideoOutput_nid_postfix(7, output);
    auto replacement = std::make_shared<Output>();
    AgcDriverRegisterVideoOutput_nid_postfix(7, replacement);
    submitFlip();
    release.Release();
    AgcDriverWaitIdle_nid_postfix();
    check(output->state->ready == 1 && replacement->state->ready == 1, "registration lifetime or FIFO was lost");
    check(output->state->failed == 0, "successful request failed");
    std::vector<std::thread> producers;
    std::array<std::exception_ptr, 4> errors{};
    for (std::size_t i = 0; i < errors.size(); ++i) {
        producers.emplace_back([&, i] {
            try {
                for (int j = 0; j < 50; ++j) {
                    submitFlip();
                    if (j % 5 == 0) AgcDriverSuspendPoint_nid_postfix();
                }
            } catch (...) { errors[i] = std::current_exception(); }
        });
    }
    for (auto& producer : producers) producer.join();
    for (auto error : errors) if (error) std::rethrow_exception(error);
    AgcDriverSuspendPoint_nid_postfix();
    AgcDriverWaitIdle_nid_postfix();
    check(replacement->state->ready == 201, "concurrent submissions were lost");
    AgcDriverUnregisterVideoOutput_nid_postfix(7, replacement);
}

void testFailure() {
    auto output = std::make_shared<Output>();
    output->state->fail = true;
    AgcDriverRegisterVideoOutput_nid_postfix(7, output);
    submitFlip();
    std::array<std::string, 4> messages;
    std::vector<std::thread> waiters;
    for (auto& message : messages) waiters.emplace_back([&message] { message = expectFailure([] { AgcDriverWaitIdle_nid_postfix(); }); });
    for (auto& waiter : waiters) waiter.join();
    for (auto& message : messages) check(message == "intentional flip failure", "asynchronous failure was lost");
    check(expectFailure([] { AgcDriverWaitIdle_nid_postfix(); }) == messages[0], "idle lost flip failure");
    check(expectFailure([] { AgcDriverSuspendPoint_nid_postfix(); }) == messages[0], "suspend lost flip failure");
    check(expectFailure([] { submitFlip(); }) == messages[0], "submit lost flip failure");
    check(output->state->ready == 0 && output->state->failed == 1, "failed flip was completed successfully");
    AgcDriverUnregisterVideoOutput_nid_postfix(7, output);
}

void testReset(bool compute) {
    std::array<std::uint32_t, 4> registers{0xc0027600, 0x20c, 1, 0};
    Packet packet{registers.data(), 4, 0, {}};
    if (compute) sceAgcDriverSubmitAcb(0x20, &packet);
    else sceAgcDriverSubmitDcb(&packet);
    AgcDriverSuspendPoint_nid_postfix();
    std::array<std::uint32_t, 5> dispatch{0xc0031500, 1, 1, 1, 0x41};
    packet = Packet{dispatch.data(), 5, 0, {}};
    if (compute) sceAgcDriverSubmitAcb(0x20, &packet);
    else sceAgcDriverSubmitDcb(&packet);
    const auto message = expectFailure([] { AgcDriverWaitIdle_nid_postfix(); });
    check(message.find(compute ? "registered" : "required shader register") != std::string::npos, "suspend reset wrong queue state");
}

}

void testThrottle() {
    auto output = std::make_shared<Output>();
    AgcDriverRegisterVideoOutput_nid_postfix(7, output);
    {
        std::lock_guard lock(output->state->mutex);
        output->state->block = true;
        output->state->entered = false;
    }
    FlipRelease release(output->state);
    submitFlip();
    {
        std::unique_lock lock(output->state->mutex);
        check(output->state->changed.wait_for(lock, std::chrono::seconds(5), [&] { return output->state->entered; }), "throttle: the worker did not reach the first flip");
    }
    submitFlip();
    auto third = std::async(std::launch::async, [] { submitFlip(); });
    check(third.wait_for(std::chrono::milliseconds(200)) == std::future_status::timeout, "throttle: a submission ran ahead of a flip the worker has not started");
    release.Release();
    check(third.wait_for(std::chrono::seconds(5)) == std::future_status::ready, "throttle: a submission stayed blocked after the queued flip started");
    third.get();
    AgcDriverWaitIdle_nid_postfix();
    check(output->state->ready == 3, "throttle: flips were lost");
    alignas(4) static std::uint32_t label = 0;
    const auto address = reinterpret_cast<std::uintptr_t>(&label);
    std::array<std::uint32_t, 7> wait{0xc0053c00, 0x13, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), 1, 0xffffffffu, 0x19};
    Packet waitPacket{wait.data(), static_cast<std::uint32_t>(wait.size()), 0, {}};
    check(sceAgcDriverSubmitDcb(&waitPacket) == 0, "throttle: the wait submission failed");
    submitFlip();
    auto behind = std::async(std::launch::async, [] { submitFlip(); });
    check(behind.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready, "throttle: a submission waited for a worker that waits for a later store");
    behind.get();
    std::atomic_ref<std::uint32_t>(label).store(1, std::memory_order_release);
    AgcDriverWaitIdle_nid_postfix();
    check(output->state->ready == 5, "throttle: flips behind the wait were lost");
    AgcDriverUnregisterVideoOutput_nid_postfix(7, output);
}

void testThrottleAcrossQueues() {
    auto output = std::make_shared<Output>();
    AgcDriverRegisterVideoOutput_nid_postfix(7, output);
    alignas(4) static std::uint32_t gate = 0;
    alignas(4) static std::uint32_t relay = 0;
    const auto gateAddress = reinterpret_cast<std::uintptr_t>(&gate);
    const auto relayAddress = reinterpret_cast<std::uintptr_t>(&relay);
    std::array<std::uint32_t, 12> producer{0xc0053c00, 0x13, static_cast<std::uint32_t>(gateAddress), static_cast<std::uint32_t>(gateAddress >> 32u), 1, 0xffffffffu, 0x19, 0xc0033700, 0x00100200, static_cast<std::uint32_t>(relayAddress), static_cast<std::uint32_t>(relayAddress >> 32u), 1};
    Packet producerPacket{producer.data(), static_cast<std::uint32_t>(producer.size()), 0, {}};
    check(sceAgcDriverSubmitAcb(0x20, &producerPacket) == 0, "throttle across queues: the compute submission failed");
    std::array<std::uint32_t, 7> consumer{0xc0053c00, 0x13, static_cast<std::uint32_t>(relayAddress), static_cast<std::uint32_t>(relayAddress >> 32u), 1, 0xffffffffu, 0x19};
    Packet consumerPacket{consumer.data(), static_cast<std::uint32_t>(consumer.size()), 0, {}};
    check(sceAgcDriverSubmitDcb(&consumerPacket) == 0, "throttle across queues: the wait submission failed");
    submitFlip();
    auto behind = std::async(std::launch::async, [] { submitFlip(); });
    const auto status = behind.wait_for(std::chrono::milliseconds(500));
    std::atomic_ref<std::uint32_t>(gate).store(1, std::memory_order_release);
    check(status == std::future_status::ready, "throttle across queues: a submission waited for a worker whose wait only a later store releases");
    behind.get();
    AgcDriverWaitIdle_nid_postfix();
    check(std::atomic_ref<std::uint32_t>(relay).load(std::memory_order_acquire) == 1 && output->state->ready == 2, "throttle across queues: work behind the waits was lost");
    AgcDriverUnregisterVideoOutput_nid_postfix(7, output);
}

int main(int argc, char** argv) {
    try {
        if (argc == 2) testReset(std::string(argv[1]) == "compute");
        else { testFlipAndBoundary(); testThrottle(); testThrottleAcrossQueues(); testFailure(); }
        const auto shutdown = expectFailure([] { LibcRunShutdown_nid_postfix(); });
        check(shutdown.find(argc == 2 ? (std::string(argv[1]) == "compute" ? "registered" : "required shader register") : "intentional flip failure") != std::string::npos, "shutdown lost worker failure");
        std::puts("AGC flip and suspend tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        try { LibcRunShutdown_nid_postfix(); }
        catch (const std::exception& shutdown) { std::fprintf(stderr, "shutdown: %s\n", shutdown.what()); }
        return 1;
    }
}
