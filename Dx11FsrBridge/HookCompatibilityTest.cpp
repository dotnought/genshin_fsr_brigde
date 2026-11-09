#include "HookCompatibility.h"

#include <cstdio>
#include <thread>

int main()
{
    int failures = 0;
    const auto expect = [&](bool ok, const char *message)
    {
        if (!ok)
        {
            std::printf("FAIL: %s\n", message);
            ++failures;
        }
    };
    using hook_compatibility::is_ordinal_export;
    expect(is_ordinal_export(nullptr), "null is not a name");
    expect(is_ordinal_export(reinterpret_cast<const char *>(5)), "ordinal 5");
    expect(is_ordinal_export(reinterpret_cast<const char *>(0xffff)), "ordinal upper boundary");
    expect(!is_ordinal_export("D3D11CreateDevice"), "named export");
    int native = 1, wrapper = 2, hook = 3;
    using hook_compatibility::can_chain_import;
    expect(can_chain_import(&native, &hook, nullptr), "first provider");
    expect(can_chain_import(&native, &hook, &native), "same provider");
    expect(can_chain_import(&hook, &hook, &native), "already installed");
    expect(!can_chain_import(&wrapper, &hook, &native), "preserve wrapped IAT");
    expect(!can_chain_import(&native, &hook, &wrapper), "preserve native IAT in reverse order");
    hook_compatibility::OriginalMethods methods;
    void *native_methods[] = { &native };
    void *wrapper_methods[] = { &wrapper };
    methods.publish(&native, native_methods, 1);
    methods.publish(&wrapper, wrapper_methods, 1);
    for (int iteration = 0; iteration < 100; ++iteration)
    {
        expect(methods.find(&native, 0) == &native, "native original stays native");
        expect(methods.find(&wrapper, 0) == &wrapper, "wrapper original stays wrapped");
    }
    expect(methods.find(&native, 1) == nullptr, "slot bounds");
    expect(methods.find(&hook, 0) == nullptr, "unknown object");
    methods.publish(&native, wrapper_methods, 1);
    expect(methods.find(&native, 0) == &wrapper, "republish invalidates cached address");
    std::atomic<bool> bad_read { false };
    std::thread reader([&]()
    {
        for (int iteration = 0; iteration < 10000; ++iteration)
        {
            void *value = methods.find(&native, 0);
            if (value != &native && value != &wrapper)
                bad_read.store(true);
        }
    });
    for (int iteration = 0; iteration < 1000; ++iteration)
        methods.publish(&native, iteration % 2 == 0 ? native_methods : wrapper_methods, 1);
    reader.join();
    expect(!bad_read.load(), "concurrent registry publication");
    int finishes = 0;
    int output = 0;
    using hook_compatibility::finish_upscale_submission;
    const auto finish = [&]() { ++finishes; output = 42; return true; };
    expect(!finish_upscale_submission(false, true, finish), "failed submission stays failed");
    expect(finishes == 0 && output == 0, "failed submission cannot publish");
    expect(finish_upscale_submission(true, true, finish), "complete async handoff");
    expect(output == 42 && finishes == 1, "consumer sees completed handoff before return");
    expect(finish_upscale_submission(true, false, finish), "sync already completed handoff");
    expect(finishes == 1, "sync does not duplicate handoff");
    expect(!finish_upscale_submission(true, true, []() { return false; }), "handoff failure propagates");
    std::printf("HookCompatibilityTest: %s\n", failures == 0 ? "ALL PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
