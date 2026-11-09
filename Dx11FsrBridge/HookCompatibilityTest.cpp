#include "HookCompatibility.h"

#include <cstdio>

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
    std::printf("HookCompatibilityTest: %s\n", failures == 0 ? "ALL PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
