// Pemindai memori PSP (native) untuk mencari alamat cheat, termasuk preset widescreen.
#pragma once

#include <cstdint>
#include <mutex>
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

// ---------------------------------------------------------------------------
// Freeze / lock: nilai ditulis ulang setiap frame supaya tidak berubah.
// Tiap entri juga menyimpan "signature" (byte di sekitar alamat) supaya alamatnya
// bisa dicari ulang setelah pindah stage (memori dinamis berpindah).
// ---------------------------------------------------------------------------
struct FreezeEntry {
	uint32_t addr = 0;
	int size = 4;
	HitKind kind = HitKind::Value;
	uint32_t value = 0;
	bool enabled = true;
	std::string label;
	std::vector<int16_t> sig;  // byte di sekitar alamat; -1 = wildcard (byte target sendiri)
};

class MemFreezer {
public:
	static int Add(uint32_t addr, int size, HitKind kind, uint32_t value, const std::string &label);
	static void Remove(int idx);
	static void ClearAll();
	static void SetEnabled(int idx, bool on);
	static void SetValue(int idx, uint32_t value);
	static int Count();
	static bool Get(int idx, FreezeEntry *out);  // salinan (aman lintas thread)
	// Dipanggil tiap frame dari thread emulasi (lihat CheatEvent di CwCheat.cpp).
	static void Apply();
	// Cari ulang alamat lewat signature. Mengembalikan true bila ketemu dan alamat diperbarui.
	// outMisses = jumlah byte yang tidak cocok (0 = persis sama), outOld/outNew = alamat lama/baru.
	static bool Relocate(int idx, int *outMisses, uint32_t *outOld, uint32_t *outNew);
	// Relokasi semua entri; mengembalikan jumlah yang berhasil.
	static int RelocateAll();
};

// ---------------------------------------------------------------------------
// Pantau akses CPU: pasang memcheck pada satu alamat, catat setiap instruksi (PC) yang
// membaca/menulisnya beserta register basis dan offset-nya. Dari "lw t1,0x12(v1)" kita tahu
// pointer dasar = alamat - 0x12, lalu bisa dicari di mana pointer itu disimpan.
// ---------------------------------------------------------------------------
struct AccessRecord {
	uint32_t pc = 0;          // alamat instruksi yang mengakses
	uint32_t addr = 0;        // alamat memori yang diakses
	int size = 0;             // ukuran akses instruksi (0 = bukan load/store biasa, mis. HLE)
	bool write = false;
	int baseReg = -1;         // register basis (rs), -1 = tidak diketahui
	int32_t offset = 0;       // immediate pada instruksi
	uint32_t baseValue = 0;   // addr - offset = nilai pointer dasar
	uint32_t count = 0;       // berapa kali instruksi ini mengakses
	std::string text;         // hasil disassembly
};

class MemWatch {
public:
	static bool Start(uint32_t addr, int size);
	static void Stop();
	static bool Active();
	static uint32_t WatchAddr();
	static int WatchSize();
	static void Clear();
	// Salinan log, diurutkan dari yang paling sering. Disassembly diisi di sini (thread UI).
	static std::vector<AccessRecord> Snapshot();
	// Dipanggil dari MemCheck::Apply (thread emulasi). start/end = rentang memcheck yang kena.
	static void Record(uint32_t start, uint32_t end, uint32_t addr, bool write, int size, uint32_t pc);
	static const char *RegName(int reg);
};
