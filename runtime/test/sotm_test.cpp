#include <cassert>
#include <cstdio>
#include <iostream>
#include "../src/sotm_accounting.h"
#include "../src/sotm_budget.h"
#include "../src/sotm_page_table.h"

int main() {
    std::printf("[test] Running SOTM unit tests...\n");

    // 1. Test Accounting
    auto& acc = leonida::sotm::Accounting::instance();
    acc.track_alloc(leonida::sotm::Category::TextureVram, 1024 * 1024 * 64); // 64 MB
    acc.track_alloc(leonida::sotm::Category::GuestHeapCommitted, 1024 * 1024 * 128); // 128 MB

    assert(acc.get_bytes(leonida::sotm::Category::TextureVram) == 1024 * 1024 * 64);
    assert(acc.get_bytes(leonida::sotm::Category::GuestHeapCommitted) == 1024 * 1024 * 128);

    acc.track_free(leonida::sotm::Category::TextureVram, 1024 * 1024 * 32);
    assert(acc.get_bytes(leonida::sotm::Category::TextureVram) == 1024 * 1024 * 32);

    leonida::VramBudget budget;
    budget.available = true;
    budget.local_usage = 1024 * 1024 * 1000;
    budget.local_budget = 1024 * 1024 * 6000;
    budget.shared_usage = 0;

    std::string json = acc.to_json(1.0, budget);
    assert(json.find("\"texture_vram\":32") != std::string::npos);
    assert(json.find("\"guest_heap_committed\":128") != std::string::npos);

    // 1b. Test symmetric VkDeviceMemory tracking across ShadowBuffers / HostImports
    acc.track_vk_alloc(0x1001, leonida::sotm::Category::ShadowBuffers, 1024 * 1024 * 48);
    acc.track_vk_alloc(0x1002, leonida::sotm::Category::HostImports, 1024 * 1024 * 12);
    acc.track_evict(1024 * 1024 * 4);
    acc.track_decommit(1024 * 1024 * 8);
    acc.track_mip_cap(1024 * 1024 * 16);
    assert(acc.get_bytes(leonida::sotm::Category::ShadowBuffers) == 1024 * 1024 * 48);
    assert(acc.get_bytes(leonida::sotm::Category::HostImports) == 1024 * 1024 * 12);
    acc.track_vk_free(0x1001);
    acc.track_vk_free(0x1001);  // idempotent double-free guard
    acc.track_vk_free(0x1002);
    assert(acc.get_bytes(leonida::sotm::Category::ShadowBuffers) == 0);
    assert(acc.get_bytes(leonida::sotm::Category::HostImports) == 0);
    std::string json_ops = acc.to_json(2.0, budget);
    assert(json_ops.find("\"shadow_buffers\":0") != std::string::npos);
    assert(json_ops.find("\"evictions\":1") != std::string::npos);
    assert(json_ops.find("\"decommits\":1") != std::string::npos);
    assert(json_ops.find("\"mip_capped\":1") != std::string::npos);
    std::printf("[test] Accounting passed. JSON: %s\n", json_ops.c_str());

    // 2. Test Page Table
    auto& pt = leonida::sotm::PageTable::instance();
    uintptr_t test_addr = 0x140000000; // Guest base
    size_t test_size = 64 * 1024 * 4; // 4 pages = 256 KiB

    pt.set_range(test_addr, test_size, leonida::sotm::PageState::HostOnly, leonida::sotm::AllocClass::UploadOnce);
    auto* p0 = pt.get_page(test_addr);
    assert(p0 != nullptr);
    assert(p0->state == leonida::sotm::PageState::HostOnly);
    assert(p0->cls == leonida::sotm::AllocClass::UploadOnce);

    // Transition to BothClean on GPU upload
    pt.mark_gpu_upload(test_addr, test_size, 42);
    p0 = pt.get_page(test_addr);
    assert(p0->state == leonida::sotm::PageState::BothClean);
    assert(p0->lastUseFrame == 42);

    // Record provenance
    uint32_t pidx = pt.record_provenance(test_addr, test_size, 101, 0x50000, 0x10000, 2);
    assert(pidx == 0);
    const auto* prov = pt.get_provenance(test_addr);
    assert(prov != nullptr);
    assert(prov->file_id == 101);
    assert(prov->offset == 0x50000);
    assert(prov->codec == 2);

    // Transition to BothDirty on CPU write
    pt.mark_cpu_write(test_addr, 64 * 1024);
    p0 = pt.get_page(test_addr);
    assert(p0->state == leonida::sotm::PageState::BothDirty);
    assert(pt.get_provenance(test_addr) == nullptr); // Invalidate provenance on write

    auto stats = pt.collect_stats();
    assert(stats.both_dirty == 1);
    assert(stats.both_clean == 3);

    // Budget pressure & adaptive mip ceiling tests
    leonida::sotm::BudgetConfig bcfg{};
    bcfg.max_vram_bytes = 5000ull << 20;
    bcfg.max_host_ram_bytes = 16000ull << 20;
    bcfg.mip_max_normal = 1024;
    leonida::VramBudget vb{};
    vb.available = true;
    vb.local_budget = 6000ull << 20;  // min(5000, 6000) = 5000 MB effective
    vb.local_usage = 2000ull << 20;
    assert(leonida::sotm::evaluate_pressure(vb, 8000ull << 20, bcfg) == leonida::sotm::PressureTier::Normal);
    vb.local_usage = 3600ull << 20;  // 72%
    assert(leonida::sotm::evaluate_pressure(vb, 8000ull << 20, bcfg) == leonida::sotm::PressureTier::Elevated);
    vb.local_usage = 4300ull << 20;  // 86%
    assert(leonida::sotm::evaluate_pressure(vb, 8000ull << 20, bcfg) == leonida::sotm::PressureTier::Critical);
    vb.local_usage = 2000ull << 20;
    assert(leonida::sotm::evaluate_pressure(vb, 15500ull << 20, bcfg) == leonida::sotm::PressureTier::OverBudget);

    assert(leonida::sotm::choose_first_mip(4096, 4096, 13, leonida::sotm::PressureTier::Normal, 1024) == 2);
    assert(leonida::sotm::choose_first_mip(4096, 4096, 13, leonida::sotm::PressureTier::Elevated, 1024) == 3);
    assert(leonida::sotm::choose_first_mip(4096, 4096, 13, leonida::sotm::PressureTier::Critical, 1024) == 4);
    assert(leonida::sotm::choose_first_mip(4096, 4096, 13, leonida::sotm::PressureTier::OverBudget, 1024) == 5);
    assert(leonida::sotm::choose_first_mip(4096, 4096, 1, leonida::sotm::PressureTier::OverBudget, 1024) == 0);

    // Phase 3 single-owner handoff, 64-bit tag isolation, and >= 3 fault ColdShared hysteresis
    const uintptr_t p3_addr = 0x200000000ULL;
    const uintptr_t p3_alias = p3_addr + (1ULL << 36);  // differs only in bit 36 (previous collision point)
    pt.record_provenance(p3_addr, 64 * 1024, 7, 0x1000, 0x1000, 1);
    assert(pt.can_decommit(p3_addr, p3_addr + 64 * 1024));
    assert(!pt.can_decommit(p3_alias, p3_alias + 64 * 1024));
    pt.mark_device_only(p3_addr, p3_addr + 64 * 1024);
    assert(pt.is_device_only(p3_addr));
    assert(!pt.is_device_only(p3_alias));
    assert(pt.recommit_on_fault(p3_addr, false));
    assert(pt.get_page(p3_addr)->faultCount == 1);
    assert(pt.get_page(p3_addr)->cls != leonida::sotm::AllocClass::ColdShared);
    pt.record_provenance(p3_addr, 64 * 1024, 7, 0x1000, 0x1000, 1);  // resets faultCount on fresh stream
    pt.mark_device_only(p3_addr, p3_addr + 64 * 1024);
    assert(pt.recommit_on_fault(p3_addr, true));  // read fault pins permanently
    assert(pt.get_page(p3_addr)->cls == leonida::sotm::AllocClass::ColdShared);
    assert(!pt.can_decommit(p3_addr, p3_addr + 64 * 1024));
    pt.invalidate_range(p3_addr, 64 * 1024);
    assert(!pt.is_device_only(p3_addr));

    std::printf("[test] Page table, handoff, and budget pressure tests passed.\n");
    std::printf("[test] All SOTM unit tests passed successfully!\n");
    return 0;
}
