// Pemindai memori PSP (native) untuk mencari alamat cheat, termasuk preset widescreen.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class ScanType { U8, U16, U32, Float };
enum class ScanCmp { Equal, Changed, Unchanged, Greater, Less };
enum class HitKind { Value, Lui };  // Lui = instruksi MIPS "lui" (immediate 16 bit di 2 byte rendah)

struct ScanHit {
	uint32_t addr;
	uint32_t value;   // nilai terakhir dibaca (mentah)
	int size;         // 1, 2, atau 4 byte
	HitKind kind;
	const char *tag;  // keterangan (hanya untuk preset widescreen)
};

class MemScanner {
public:
	void Reset();
	// Scan pertama: cari nilai persis (float memakai toleransi kecil).
	void First(ScanType type, uint32_t rawValue);
	// Scan lanjutan dari hasil sebelumnya.
	void Next(ScanCmp cmp, uint32_t rawValue);
	// Preset: cari konstanta layar 4:3 / 16:9 (float dan instruksi lui).
	void ScanWidescreen();

	const std::vector<ScanHit> &Hits() const { return hits_; }
	bool Truncated() const { return truncated_; }
	bool IsWidescreenMode() const { return wide_; }
	ScanType Type() const { return type_; }
	bool HasScan() const { return hasScan_; }

	static uint32_t ReadAt(uint32_t addr, int size);
	// Tulis ke memori PSP dan batalkan cache JIT (penting untuk patch instruksi).
	static bool WriteAt(uint32_t addr, int size, uint32_t value);
	static int SizeOf(ScanType t);

private:
	std::vector<ScanHit> hits_;
	ScanType type_ = ScanType::Float;
	bool truncated_ = false;
	bool wide_ = false;
	bool hasScan_ = false;
};
