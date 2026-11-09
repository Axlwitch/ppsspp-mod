#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

#include "Core/MemScan.h"
#include "Core/MemMap.h"
#include "Core/MIPS/MIPS.h"
#include "Core/MIPS/MIPSAnalyst.h"
#include "Core/MIPS/MIPSDebugInterface.h"
#include "Core/Debugger/Breakpoints.h"
#include "Core/Debugger/DisassemblyManager.h"
#include <map>
#include <atomic>

static const uint32_t SCAN_START = 0x08800000;  // awal RAM user (8 MB pertama = kernel)
// Akhir RAM mengikuti ukuran RAM yang aktif: PSP-1000 = 32 MB -> 0x0A000000 (user 24 MB),
// PSP-2000+ = 64 MB -> 0x0C000000. (Pastikan nama Memory::g_MemorySize ada di versimu.)
static uint32_t ScanEnd() { return 0x08000000u + Memory::g_MemorySize; }
static const uint32_t BLOCK = 0x10000;
static const size_t MAX_HITS = 300000;

int MemScanner::SizeOf(ScanType t) {
	switch (t) {
	case ScanType::U8: return 1;
	case ScanType::U16: return 2;
	default: return 4;
	}
}

uint32_t MemScanner::ReadAt(uint32_t addr, int size) {
	if (!Memory::IsValidRange(addr, size))
		return 0;
	if (size == 1) return Memory::ReadUnchecked_U8(addr);
	if (size == 2) return Memory::ReadUnchecked_U16(addr);
	return Memory::ReadUnchecked_U32(addr);
}

bool MemScanner::WriteAt(uint32_t addr, int size, uint32_t value) {
	if (!Memory::IsValidRange(addr, size))
		return false;
	if (size == 1) Memory::WriteUnchecked_U8((uint8_t)value, addr);
	else if (size == 2) Memory::WriteUnchecked_U16((uint16_t)value, addr);
	else Memory::WriteUnchecked_U32(value, addr);
	if (currentMIPS)
		currentMIPS->InvalidateICacheRangeDeferred(addr & ~3u, 8);
	return true;
}

static float AsFloat(uint32_t raw) {
	float f;
	memcpy(&f, &raw, 4);
	return f;
}

static bool FloatNear(uint32_t a, uint32_t b) {
	float fa = AsFloat(a), fb = AsFloat(b);
	if (std::isnan(fa) || std::isnan(fb))
		return a == b;
	return std::fabs(fa - fb) <= 1e-5f * std::max(1.0f, std::fabs(fb));
}

static bool Matches(ScanType t, ScanCmp cmp, uint32_t cur, uint32_t prev, uint32_t target) {
	switch (cmp) {
	case ScanCmp::Equal:
		return t == ScanType::Float ? FloatNear(cur, target) : cur == target;
	case ScanCmp::Changed:
		return cur != prev;
	case ScanCmp::Unchanged:
		return cur == prev;
	case ScanCmp::Greater:
		return t == ScanType::Float ? AsFloat(cur) > AsFloat(prev) : cur > prev;
	case ScanCmp::Less:
		return t == ScanType::Float ? AsFloat(cur) < AsFloat(prev) : cur < prev;
	}
	return false;
}

void MemScanner::Reset() {
	hits_.clear();
	hits_.shrink_to_fit();
	truncated_ = false;
	wide_ = false;
	hasScan_ = false;
}

void MemScanner::First(ScanType type, uint32_t rawValue) {
	Reset();
	type_ = type;
	hasScan_ = true;
	const int size = SizeOf(type);
	for (uint64_t base = SCAN_START; base < ScanEnd() && !truncated_; base += BLOCK) {
		if (!Memory::IsValidRange((uint32_t)base, BLOCK))
			continue;
		for (uint32_t off = 0; off < BLOCK; off += size) {
			uint32_t addr = (uint32_t)base + off;
			uint32_t cur = ReadAt(addr, size);
			if (Matches(type, ScanCmp::Equal, cur, 0, rawValue)) {
				hits_.push_back({ addr, cur, size, HitKind::Value, nullptr });
				if (hits_.size() >= MAX_HITS) {
					truncated_ = true;
					break;
				}
			}
		}
	}
}

void MemScanner::Next(ScanCmp cmp, uint32_t rawValue) {
	if (!hasScan_ || wide_)
		return;
	std::vector<ScanHit> kept;
	kept.reserve(hits_.size());
	for (ScanHit h : hits_) {
		uint32_t cur = ReadAt(h.addr, h.size);
		if (Matches(type_, cmp, cur, h.value, rawValue)) {
			h.value = cur;
			kept.push_back(h);
		}
	}
	hits_.swap(kept);
}

struct WideConst {
	uint32_t bits;
	uint16_t lui;
	const char *tag;
};

// Konstanta rasio layar yang umum dipatch untuk widescreen.
static const WideConst kWideConsts[] = {
	{ 0x3FAAAAAB, 0x3FAB, "4:3 (1.3333)" },
	{ 0x3FAAAAAB, 0x3FAA, "4:3 (1.3333)" },
	{ 0x3F400000, 0x3F40, "3/4 (0.75)" },
	{ 0x3F100000, 0x3F10, "9/16 (0.5625)" },
	{ 0x3FE38E39, 0x3FE3, "16:9 (1.7778)" },
	{ 0x3FE1E1E2, 0x3FE1, "480/272 (1.7647)" },
};

void MemScanner::ScanWidescreen() {
	Reset();
	hasScan_ = true;
	wide_ = true;
	type_ = ScanType::U32;
	for (uint64_t base = SCAN_START; base < ScanEnd() && !truncated_; base += BLOCK) {
		if (!Memory::IsValidRange((uint32_t)base, BLOCK))
			continue;
		for (uint32_t off = 0; off < BLOCK; off += 4) {
			uint32_t addr = (uint32_t)base + off;
			uint32_t w = Memory::ReadUnchecked_U32(addr);
			// 1) float data mentah
			for (const WideConst &c : kWideConsts) {
				if (w == c.bits) {
					hits_.push_back({ addr, w, 4, HitKind::Value, c.tag });
					break;
				}
			}
			// 2) instruksi "lui rt, imm" (opcode 001111, rs = 0)
			if ((w & 0xFFE00000u) == 0x3C000000u) {
				uint16_t imm = (uint16_t)(w & 0xFFFF);
				for (const WideConst &c : kWideConsts) {
					if (imm == c.lui) {
						hits_.push_back({ addr, w, 2, HitKind::Lui, c.tag });
						break;
					}
				}
			}
			if (hits_.size() >= MAX_HITS) {
				truncated_ = true;
				break;
			}
		}
	}
}

// ===========================================================================
// MemFreezer
// ===========================================================================
static std::mutex g_freezeLock;
static std::vector<FreezeEntry> g_freezes;
static const int SIG_BEFORE = 16;
static const int SIG_AFTER = 16;

// Ambil byte di sekitar alamat. Byte target sendiri dijadikan wildcard (-1) karena nilainya berubah.
static void CaptureSig(FreezeEntry &e) {
	e.sig.clear();
	for (int i = -SIG_BEFORE; i < e.size + SIG_AFTER; i++) {
		uint32_t a = e.addr + i;
		bool isTarget = i >= 0 && i < e.size;
		if (isTarget || a < SCAN_START || !Memory::IsValidRange(a, 1))
			e.sig.push_back(-1);
		else
			e.sig.push_back((int16_t)Memory::ReadUnchecked_U8(a));
	}
}

int MemFreezer::Add(uint32_t addr, int size, HitKind kind, uint32_t value, const std::string &label) {
	FreezeEntry e;
	e.addr = addr;
	e.size = size;
	e.kind = kind;
	e.value = value;
	e.label = label;
	e.enabled = true;
	CaptureSig(e);
	std::lock_guard<std::mutex> guard(g_freezeLock);
	// Alamat sama -> perbarui entri lama, jangan dobel.
	for (size_t i = 0; i < g_freezes.size(); i++) {
		if (g_freezes[i].addr == addr && g_freezes[i].size == size) {
			g_freezes[i] = e;
			return (int)i;
		}
	}
	g_freezes.push_back(e);
	return (int)g_freezes.size() - 1;
}

void MemFreezer::Remove(int idx) {
	std::lock_guard<std::mutex> guard(g_freezeLock);
	if (idx >= 0 && idx < (int)g_freezes.size())
		g_freezes.erase(g_freezes.begin() + idx);
}

void MemFreezer::ClearAll() {
	std::lock_guard<std::mutex> guard(g_freezeLock);
	g_freezes.clear();
}

void MemFreezer::SetEnabled(int idx, bool on) {
	std::lock_guard<std::mutex> guard(g_freezeLock);
	if (idx >= 0 && idx < (int)g_freezes.size())
		g_freezes[idx].enabled = on;
}

void MemFreezer::SetValue(int idx, uint32_t value) {
	std::lock_guard<std::mutex> guard(g_freezeLock);
	if (idx >= 0 && idx < (int)g_freezes.size())
		g_freezes[idx].value = value;
}

int MemFreezer::Count() {
	std::lock_guard<std::mutex> guard(g_freezeLock);
	return (int)g_freezes.size();
}

bool MemFreezer::Get(int idx, FreezeEntry *out) {
	std::lock_guard<std::mutex> guard(g_freezeLock);
	if (idx < 0 || idx >= (int)g_freezes.size())
		return false;
	*out = g_freezes[idx];
	return true;
}

void MemFreezer::Apply() {
	std::lock_guard<std::mutex> guard(g_freezeLock);
	if (g_freezes.empty() || !Memory::IsActive())
		return;
	for (const FreezeEntry &e : g_freezes) {
		if (!e.enabled || !Memory::IsValidRange(e.addr, e.size))
			continue;
		// Tulis hanya jika berbeda (hemat, dan tidak membatalkan cache JIT terus-menerus).
		if (ReadAt(e.addr, e.size) == e.value)
			continue;
		if (e.kind == HitKind::Lui)
			WriteAt(e.addr, e.size, e.value);  // dengan invalidate JIT
		else if (e.size == 1)
			Memory::WriteUnchecked_U8((uint8_t)e.value, e.addr);
		else if (e.size == 2)
			Memory::WriteUnchecked_U16((uint16_t)e.value, e.addr);
		else
			Memory::WriteUnchecked_U32(e.value, e.addr);
	}
}

// Cari lokasi yang byte sekitarnya paling mirip dengan signature.
// Toleransi 25% byte berbeda (struktur di sekitar data biasanya ikut berubah sedikit).
static bool FindBySig(const FreezeEntry &e, uint32_t *bestAddr, int *bestMiss) {
	const uint32_t start = SCAN_START, end = ScanEnd();
	if (e.sig.empty() || !Memory::IsActive() || !Memory::IsValidRange(start, end - start))
		return false;
	const uint8_t *ram = Memory::GetPointerUnchecked(start);
	const int n = (int)e.sig.size();
	int fixedCount = 0;
	for (int16_t b : e.sig)
		if (b >= 0) fixedCount++;
	if (fixedCount < 8)
		return false;  // terlalu sedikit info, hasil pasti ngawur
	const int allowed = fixedCount / 4;

	bool found = false;
	int bMiss = allowed + 1;
	uint32_t bAddr = 0;
	uint32_t bDist = 0xFFFFFFFFu;
	const uint32_t lo = start + SIG_BEFORE;
	const uint32_t hi = end - e.size - SIG_AFTER;
	for (uint32_t a = (lo + e.size - 1) & ~(uint32_t)(e.size - 1); a <= hi; a += e.size) {
		const uint8_t *p = ram + (a - start) - SIG_BEFORE;
		int miss = 0;
		for (int i = 0; i < n; i++) {
			if (e.sig[i] >= 0 && p[i] != (uint8_t)e.sig[i]) {
				if (++miss > bMiss)  // sudah lebih buruk dari kandidat terbaik
					break;
			}
		}
		if (miss > bMiss || miss > allowed)
			continue;
		uint32_t dist = a > e.addr ? a - e.addr : e.addr - a;
		if (!found || miss < bMiss || (miss == bMiss && dist < bDist)) {
			found = true;
			bMiss = miss;
			bAddr = a;
			bDist = dist;
		}
	}
	if (found) {
		*bestAddr = bAddr;
		*bestMiss = bMiss;
	}
	return found;
}

bool MemFreezer::Relocate(int idx, int *outMisses, uint32_t *outOld, uint32_t *outNew) {
	FreezeEntry e;
	if (!Get(idx, &e))
		return false;
	uint32_t newAddr = 0;
	int miss = 0;
	if (!FindBySig(e, &newAddr, &miss))
		return false;
	if (outMisses) *outMisses = miss;
	if (outOld) *outOld = e.addr;
	if (outNew) *outNew = newAddr;
	e.addr = newAddr;
	CaptureSig(e);  // segarkan signature di lokasi baru supaya bisa ikut berpindah lagi
	std::lock_guard<std::mutex> guard(g_freezeLock);
	if (idx < (int)g_freezes.size()) {
		bool wasEnabled = g_freezes[idx].enabled;
		g_freezes[idx] = e;
		g_freezes[idx].enabled = wasEnabled;
	}
	return true;
}

int MemFreezer::RelocateAll() {
	int ok = 0;
	for (int i = 0, n = Count(); i < n; i++) {
		if (Relocate(i, nullptr, nullptr, nullptr))
			ok++;
	}
	return ok;
}

// ===========================================================================
// MemWatch
// ===========================================================================
static std::mutex g_watchLock;
static std::atomic<bool> g_watchActive{ false };
static uint32_t g_watchStart = 0, g_watchEnd = 0;
static std::map<uint64_t, AccessRecord> g_records;  // kunci: (pc << 1) | write
static const size_t MAX_RECORDS = 2000;

bool MemWatch::Start(uint32_t addr, int size) {
	if (size <= 0 || !Memory::IsValidRange(addr, size))
		return false;
	Stop();
	{
		std::lock_guard<std::mutex> guard(g_watchLock);
		g_watchStart = addr;
		g_watchEnd = addr + size;
		g_records.clear();
		g_watchActive = true;
	}
	// BREAK_ACTION_NONE: tidak pause dan tidak menulis log; kita hanya butuh hook di MemCheck::Apply.
	// (Jika log kosong padahal alamat jelas diakses, coba ganti ke BREAK_ACTION_LOG.)
	g_breakpoints.AddMemCheck(g_watchStart, g_watchEnd, MEMCHECK_READWRITE, BREAK_ACTION_NONE);
	return true;
}

void MemWatch::Stop() {
	uint32_t s, e;
	{
		std::lock_guard<std::mutex> guard(g_watchLock);
		if (!g_watchActive)
			return;
		g_watchActive = false;
		s = g_watchStart;
		e = g_watchEnd;
	}
	g_breakpoints.RemoveMemCheck(s, e);
}

bool MemWatch::Active() { return g_watchActive; }

uint32_t MemWatch::WatchAddr() {
	std::lock_guard<std::mutex> guard(g_watchLock);
	return g_watchStart;
}

int MemWatch::WatchSize() {
	std::lock_guard<std::mutex> guard(g_watchLock);
	return (int)(g_watchEnd - g_watchStart);
}

void MemWatch::Clear() {
	std::lock_guard<std::mutex> guard(g_watchLock);
	g_records.clear();
}

const char *MemWatch::RegName(int r) {
	static const char *const names[32] = {
		"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
		"s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra",
	};
	return (r >= 0 && r < 32) ? names[r] : "?";
}

void MemWatch::Record(uint32_t start, uint32_t end, uint32_t addr, bool write, int size, uint32_t pc) {
	if (!g_watchActive)
		return;
	std::lock_guard<std::mutex> guard(g_watchLock);
	if (start != g_watchStart || end != g_watchEnd)
		return;  // memcheck milik orang lain (debugger), bukan pantauan kita

	// Cari instruksi yang benar-benar mengakses (sama dengan ExecOpMemCheck: kasus delay slot).
	uint32_t ipc = pc;
	int asz = 0;
	if (Memory::IsValidRange(ipc, 4)) {
		asz = MIPSAnalyst::OpMemoryAccessSize(ipc);
		if (asz == 0 && MIPSAnalyst::OpHasDelaySlot(ipc) && Memory::IsValidRange(ipc + 4, 4)) {
			ipc += 4;
			asz = MIPSAnalyst::OpMemoryAccessSize(ipc);
		}
	}

	const uint64_t key = ((uint64_t)ipc << 1) | (write ? 1 : 0);
	auto it = g_records.find(key);
	if (it != g_records.end()) {
		it->second.count++;
		it->second.addr = addr;
		return;
	}
	if (g_records.size() >= MAX_RECORDS)
		return;

	AccessRecord r;
	r.pc = ipc;
	r.addr = addr;
	r.size = asz;
	r.write = write;
	r.count = 1;
	if (asz > 0) {
		const uint32_t op = Memory::ReadUnchecked_U32(ipc);
		const uint32_t opc = op >> 26;
		r.baseReg = (int)((op >> 21) & 31);
		// VFPU lv.s/sv.s/lv.q/sv.q memakai offset 14 bit << 2; lainnya 16 bit bertanda.
		if (opc == 0x32 || opc == 0x3A || opc == 0x36 || opc == 0x3E)
			r.offset = (int16_t)(op & 0xFFFC);
		else
			r.offset = (int16_t)(op & 0xFFFF);
		r.baseValue = addr - (uint32_t)r.offset;
	}
	g_records[key] = r;
}

std::vector<AccessRecord> MemWatch::Snapshot() {
	std::vector<AccessRecord> out;
	{
		std::lock_guard<std::mutex> guard(g_watchLock);
		out.reserve(g_records.size());
		for (auto &kv : g_records)
			out.push_back(kv.second);
	}
	// Disassembly di thread UI (DisassemblyManager tidak dipakai thread emulasi).
	for (AccessRecord &r : out) {
		DisassemblyLineInfo line;
		g_disassemblyManager.getLine(r.pc, true, line, currentDebugMIPS);
		r.text = line.name + (line.params.empty() ? "" : " " + line.params);
	}
	std::sort(out.begin(), out.end(), [](const AccessRecord &a, const AccessRecord &b) {
		return a.count > b.count;
	});
	return out;
}
