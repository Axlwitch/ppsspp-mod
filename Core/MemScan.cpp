#include <algorithm>
#include <cmath>
#include <cstring>

#include "Core/MemScan.h"
#include "Core/MemMap.h"
#include "Core/MIPS/MIPS.h"

static const uint32_t SCAN_START = 0x08800000;  // awal RAM user
static const uint32_t SCAN_END = 0x0C000000;    // batas (PSP-2000+ bisa lebih dari 24 MB)
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
	for (uint64_t base = SCAN_START; base < SCAN_END && !truncated_; base += BLOCK) {
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
	for (uint64_t base = SCAN_START; base < SCAN_END && !truncated_; base += BLOCK) {
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
