// NFSU2DrawAll.asi - Need for Speed Underground 2 
// Load city sections and renders loaded scenery sections.
// Configurations: NFSU2DrawALL.ini
// Log: NFSU2DrawALL.log

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>

// ------------------------------------------------------------------ live state
// Exported so nfsu2_live_tool.py can find it (module export "g_state").
#pragma pack(push, 1)
struct ModState {
    char magic[8];          // "NFSU2DA"
    uint32_t version;       // layout version
    uint32_t drawAll;       // 1 = main view draws every loaded section (live toggle)
    int32_t lastZone;       // id of the last zone the camera was in, -1 = none yet
    uint8_t skip[352];      // bit per section id (2800): never drawn by DrawAll
    uint8_t home[352];      // bit per section id: hidden while the camera is in its district
};
#pragma pack(pop)
extern "C" __declspec(dllexport) ModState g_state = {"NFSU2DA", 1, 0, -1, {0}, {0}};

// ------------------------------------------------------------------ game addresses
static const uintptr_t SCENERY_TABLE = 0x883EC0;   // id -> registry entry (entry+4 = section)
static const unsigned SCENERY_IDS = 0xAF0;         // 2800
static const uintptr_t ZONE_MANAGER = 0x883E70;    // this for FindZone
static const uintptr_t SECTION_LIST = 0x883E80;    // sentinel of the 0x34153 visibility-record list
static const unsigned EXTRA_NODES = 2048;          // extra pool block descriptors (orig 0xC0)
static const unsigned EXTRA_SCENERY = 2048;        // extra scenery registry entries (orig 512)
static const unsigned POOL_OBJ_SIZE = 0xF38;       // "Track Streaming" pool object
static const unsigned RENDER_LIST_ENTRIES = 40000; // orig static list holds 4200
static const uint32_t FRAME_POOL_BYTES = 0x200000; // per-frame matrix pool (orig 0x64000)

// absolute targets used from inline asm (indirect jmp/call through these)
static DWORD kGetTime = 0x57A1A0;        // cdecl, float in ST0
static DWORD kUpdResume = 0x5E86E9;      // UpdatePosition after the displaced prologue
static DWORD kResetFields = 0x5DA890;    // UnloadAll tail
static DWORD kDrawVisibleResume = 0x5E99C6;

typedef void(__fastcall *StartLoading_t)(void *streamer, int, short *ids);   // thiscall, ret 4
typedef void(__fastcall *Thiscall0_t)(void *streamer);
typedef void *(__fastcall *FindZone_t)(void *mgr, int, const float *pos);    // thiscall, ret 4
typedef void(__fastcall *SectionDraw_t)(void *section, int, void *view);     // thiscall, ret 4
static const StartLoading_t StartLoading = (StartLoading_t)0x5E7AF0;
static const Thiscall0_t WaitLoaded = (Thiscall0_t)0x5E79D0;
static const Thiscall0_t ForceRefresh = (Thiscall0_t)0x5E9650;   // determine sections from cars, load, wait
static const FindZone_t FindZone = (FindZone_t)0x5E00D0;
static const SectionDraw_t SectionDraw = (SectionDraw_t)0x5D69B0;

// ------------------------------------------------------------------ buffers
static DWORD g_preloadedFor;                                          // section array already preloaded
static __declspec(align(16)) uint8_t g_sceneryBuf[EXTRA_SCENERY * 0x1C];
static __declspec(align(16)) uint8_t g_renderList[RENDER_LIST_ENTRIES * 12];

static bool Bit(const uint8_t *bits, unsigned i) { return (bits[i >> 3] >> (i & 7)) & 1; }

// ------------------------------------------------------------------ streaming hooks
// complete(this): phase == 0 && (#needed sections not active) <= postponed (+0x28).
// Replaces every "active == needed" loading-complete test. Keeps ecx.
static __declspec(naked) void Complete()
{
    __asm {
        push esi
        push edi
        xor edi, edi
        cmp dword ptr [ecx+0x168], 0
        jne c_no
        mov edx, [ecx+0x298]
        lea eax, [ecx+0x29C]
    c_loop:
        test edx, edx
        jle c_chk
        mov esi, [eax]
        cmp dword ptr [esi+0xC], 4
        je c_next
        inc edi
    c_next:
        add eax, 4
        dec edx
        jmp c_loop
    c_chk:
        cmp edi, [ecx+0x28]
        jg c_no
        mov eax, 1
        pop edi
        pop esi
        ret
    c_no:
        xor eax, eax
        pop edi
        pop esi
        ret
    }
}

// needed-list reset (0x5D60C0): keep the pin bit 0x80 instead of zeroing the mask
static __declspec(naked) void ClearNeeded()
{
    __asm {
        mov esi, [edx]
        and byte ptr [esi+0xB], 0x80
        ret
    }
}

// ActivateSection (0x5DAA90): state 4 + pin bit
static __declspec(naked) void Activate()
{
    __asm {
        mov dword ptr [esi+0xC], 4
        or byte ptr [esi+0xB], 0x80
        ret
    }
}

// FreeSection (0x5DAB22): really freed -> unpin
static __declspec(naked) void FreeSection()
{
    __asm {
        mov [esi+0xC], ebx
        mov [esi+0x3C], ebx
        and byte ptr [esi+0xB], 0x7F
        ret
    }
}

// UnloadAll tail (0x5DB0D9): forget which region was preloaded
static __declspec(naked) void UnloadAllTail()
{
    __asm {
        mov g_preloadedFor, 0
        jmp dword ptr [kResetFields]
    }
}

// Visit every visibility record once: load what it needs, then return to the car position.
static void __fastcall Preload(void *streamer)
{
    uint8_t *rec = *(uint8_t **)SECTION_LIST;
    while (rec && (uintptr_t)rec != SECTION_LIST) {
        short ids[2] = {*(short *)(rec + 8), 0};
        StartLoading(streamer, 0, ids);
        WaitLoaded(streamer);
        rec = *(uint8_t **)rec;
    }
    ForceRefresh(streamer);
}

// UpdatePosition prologue (0x5E86E0): on the first frame of a region preload everything.
static __declspec(naked) void UpdatePositionHook()
{
    __asm {
        cmp byte ptr [ecx+0x289], 0      // streaming disabled?
        jne u_orig
        cmp dword ptr [ecx+0x69C], 0     // pool allocated?
        je u_orig
        mov eax, [ecx]                   // section array of the current region
        cmp eax, g_preloadedFor
        je u_orig
        mov g_preloadedFor, eax
        push ecx
        call Preload                     // __fastcall(ecx)
        pop ecx
    u_orig:
        push ecx                         // displaced prologue
        push esi
        mov esi, ecx
        call dword ptr [kGetTime]
        jmp dword ptr [kUpdResume]
    }
}

// InitMemoryPool (0x5E4EE0): append EXTRA_NODES block descriptors to the enlarged pool object
static __declspec(naked) void PoolNodes()
{
    __asm {
        mov [esi+0x6A4], eax             // displaced
        test eax, eax
        je n_done
        push ebx
        push edi
        lea edi, [eax+0xF38]             // POOL_OBJ_SIZE
        mov ebx, 2048                    // EXTRA_NODES
    n_loop:
        mov edx, [eax+0x10]
        mov [edx], edi
        mov [eax+0x10], edi
        lea ecx, [eax+0xC]
        mov [edi], ecx
        mov [edi+4], edx
        add edi, 0x14
        dec ebx
        jne n_loop
        pop edi
        pop ebx
    n_done:
        mov ecx, [esp+8]                 // displaced "mov ecx,[esp+4]"
        ret
    }
}

// Scenery registry ctor tail (0x5E6486): append EXTRA_SCENERY free entries
static __declspec(naked) void SceneryEntries()
{
    __asm {
        lea edx, [esi+0x6414]
        mov ecx, 2048                    // EXTRA_SCENERY
        mov eax, offset g_sceneryBuf
    s_loop:
        mov edi, [edx+4]
        mov [edi], eax
        mov [edx+4], eax
        mov [eax], edx
        mov [eax+4], edi
        add eax, 0x1C
        dec ecx
        jne s_loop
        pop edi                          
        mov eax, esi
        pop esi
        pop ebx
        ret
    }
}

// ------------------------------------------------------------------ draw-all
// Main view: every loaded scenery section (FUN_005D69B0 still frustum-culls each one).
static void __stdcall DrawAllMain(uint8_t *view)
{
    uint8_t *cam = *(uint8_t **)(*(uint8_t **)(view + 0x40) + 0x40);
    uint8_t *zone = (uint8_t *)FindZone((void *)ZONE_MANAGER, 0, (const float *)(cam + 0x40));
    if (zone)
        g_state.lastZone = *(short *)(zone + 8);
    unsigned district = (unsigned)g_state.lastZone / 100;     // -1 -> no district
    uint8_t **table = (uint8_t **)SCENERY_TABLE;
    for (unsigned i = 0; i < SCENERY_IDS; i++) {
        if (Bit(g_state.skip, i) || (Bit(g_state.home, i) && i / 100 == district))
            continue;
        uint8_t *entry = table[i];
        uint8_t *section = entry ? *(uint8_t **)(entry + 4) : nullptr;
        if (section && *(void **)(section + 0x28))
            SectionDraw(section, 0, view);
    }
}

// DrawVisibleSections (0x5E99C0) entry
static __declspec(naked) void DrawVisibleHook()
{
    __asm {
        cmp g_state.drawAll, 0
        je d_orig
        mov eax, [esp+4]
        mov eax, [eax+0x40]
        cmp dword ptr [eax+4], 1         // main player view
        jne d_orig
        jmp DrawAllMain                  // __stdcall(view), ret 4
    d_orig:
        sub esp, 0x100                   // displaced
        jmp dword ptr [kDrawVisibleResume]
    }
}

// ------------------------------------------------------------------ patching
static FILE *g_log;

static void Log(const char *fmt, ...)
{
    if (!g_log)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

struct Patch {
    uintptr_t va;
    std::vector<uint8_t> orig, repl;
};

static std::vector<uint8_t> Hex(const char *s)
{
    std::vector<uint8_t> out;
    int hi = -1;
    for (; *s; s++) {
        int v = (*s >= '0' && *s <= '9') ? *s - '0' : (*s | 0x20) >= 'a' && (*s | 0x20) <= 'f' ? (*s | 0x20) - 'a' + 10 : -1;
        if (v < 0)
            continue;
        if (hi < 0)
            hi = v;
        else
            out.push_back((uint8_t)(hi << 4 | v)), hi = -1;
    }
    return out;
}

static void Rel32(std::vector<uint8_t> &b, uintptr_t next, const void *target)
{
    int32_t r = (int32_t)((uintptr_t)target - next);
    b.insert(b.end(), (uint8_t *)&r, (uint8_t *)&r + 4);
}

static void U32(std::vector<uint8_t> &b, uint32_t v) { b.insert(b.end(), (uint8_t *)&v, (uint8_t *)&v + 4); }

// opcode (E8 call / E9 jmp) at va to target, NOP-padded to the original length
static Patch Branch(uintptr_t va, const char *orig, uint8_t op, const void *target)
{
    Patch p{va, Hex(orig), {op}};
    Rel32(p.repl, va + 5, target);
    return p;
}

class Group {
public:
    explicit Group(const char *name) : name_(name) {}
    void Add(Patch p) { patches_.push_back(p); }
    void Add(uintptr_t va, const char *orig, std::vector<uint8_t> repl) { patches_.push_back({va, Hex(orig), repl}); }

    // all-or-nothing: every site must still hold the original bytes
    bool Apply()
    {
        for (auto &p : patches_) {
            if (p.repl.size() > p.orig.size() || memcmp((void *)p.va, p.orig.data(), p.orig.size()) != 0) {
                Log("[%s] NOT applied: unexpected bytes at %08X (modified EXE or another mod?)", name_, (unsigned)p.va);
                return false;
            }
        }
        for (auto &p : patches_) {
            std::vector<uint8_t> b = p.repl;
            b.resize(p.orig.size(), 0x90);
            DWORD old;
            VirtualProtect((void *)p.va, b.size(), PAGE_EXECUTE_READWRITE, &old);
            memcpy((void *)p.va, b.data(), b.size());
            VirtualProtect((void *)p.va, b.size(), old, &old);
        }
        FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
        Log("[%s] applied (%u sites)", name_, (unsigned)patches_.size());
        return true;
    }

private:
    const char *name_;
    std::vector<Patch> patches_;
};

// ------------------------------------------------------------------ settings
static char g_ini[MAX_PATH];

static int IniInt(const char *sec, const char *key, int def) { return GetPrivateProfileIntA(sec, key, def, g_ini); }

// "A99, C97 ..." -> bits
static void IniSections(const char *sec, const char *key, const char *def, uint8_t *bits, const char *what)
{
    char buf[4096];
    GetPrivateProfileStringA(sec, key, def, buf, sizeof buf, g_ini);
    char list[4096] = "";
    char *ctx = nullptr;
    for (char *t = strtok_s(buf, " ,;\t", &ctx); t; t = strtok_s(nullptr, " ,;\t", &ctx)) {
        char c = (char)(t[0] & ~0x20);
        int n = atoi(t + 1);
        if (c < 'A' || c > 'Z' || n < 0 || n > 99 || !t[1]) {
            Log("  %s: ignored '%s'", what, t);
            continue;
        }
        unsigned id = (unsigned)(c - 'A' + 1) * 100 + n;
        if (id < SCENERY_IDS) {
            bits[id >> 3] |= 1 << (id & 7);
            sprintf_s(list + strlen(list), sizeof list - strlen(list), " %c%02d", c, n);
        }
    }
    Log("  %s:%s", what, list[0] ? list : " none");
}

static const char *DEFAULT_SKIP = "A99,C97,C98,C99,E94,E96,E97,F99,G97,G98,G99,H99,J98,N99,P99";
static const char *DEFAULT_HOME = "A98";

// slot pools sized for ~60 active sections; exhausted pools return NULL -> crash
struct SlotPool { uintptr_t countImm; uint32_t orig, count; const char *name; };
static const SlotPool SLOT_POOLS[] = {
    {0x48B1F8, 128, 1024, "TexturePackSlotPool"},
    {0x5D253A, 1024, 8192, "g_textureHeaderPool"},
    {0x488EF8, 5000, 20000, "eModelSlotPool"},
    {0x611048, 160, 1280, "WorldModelSlotPool"},
    {0x5BB708, 220, 1760, "eLightMaterialPlatInfoSlotPool"},
    {0x5BCFF8, 256, 2048, "eAnimTextureSlotPool"},
    {0x5B4277, 256, 2048, "VirginSmokeableSlotPool"},
    {0x60BC78, 224, 1792, "SpaceNodeSlotPool"},
};

static void Install(HMODULE self)
{
    char dir[MAX_PATH];
    GetModuleFileNameA(self, dir, MAX_PATH);
    char *slash = strrchr(dir, '\\');
    if (slash)
        slash[1] = 0;
    sprintf_s(g_ini, "%sNFSU2DrawAll.ini", dir);
    char logPath[MAX_PATH];
    sprintf_s(logPath, "%sNFSU2DrawAll.log", dir);
    fopen_s(&g_log, logPath, "w");
    Log("NFSU2DrawAll: settings %s", g_ini);

    // the analysed SPEED2.EXE? (mapped at 0x400000, original bytes of the draw entry)
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void *)0x5E99C0, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
        memcmp((void *)0x5E99C0, "\x81\xEC\x00\x01\x00\x00\x53", 7) != 0) {
        Log("SPEED2.EXE is not the original v1.2 build (or already patched): nothing done.");
        return;
    }

    // ---- 1. whole city in memory
    if (IniInt("Streaming", "PreloadWholeCity", 1)) {
        Group g("PreloadWholeCity");
        int poolKB = IniInt("Streaming", "PoolSizeKB", 120000);
        if (poolKB > 0) {
            std::vector<uint8_t> t;
            U32(t, poolKB), U32(t, poolKB), U32(t, poolKB);
            g.Add(0x79DC54, "60ea0000 f8a70000 a08c0000", t);
        }
        for (auto &s : SLOT_POOLS) {
            std::vector<uint8_t> o, n;
            U32(o, s.orig), U32(n, s.count);
            Patch p{s.countImm, o, n};
            g.Add(p);
        }
        g.Add(Branch(0x5DAA90, "c7460c04000000", 0xE8, Activate));
        g.Add(Branch(0x5D60C0, "8b32 885e0b", 0xE8, ClearNeeded));
        g.Add(Branch(0x5DAB22, "895e0c 895e3c", 0xE8, FreeSection));
        g.Add(Branch(0x5D6160, "8b8168010000", 0xE9, Complete));
        {   // WaitLoaded loop condition
            Patch p{0x5E79D4, Hex("8b8768010000 85c0 7512 8b4728 8b5724 8b8f98020000 03c2 3bc1 743f"), Hex("8BCF E8")};
            Rel32(p.repl, 0x5E79DB, Complete);
            auto t = Hex("85C0 7550");
            p.repl.insert(p.repl.end(), t.begin(), t.end());
            g.Add(p);
        }
        {   // BlockUntilLoaded: three copies of the condition
            Patch p{0x5E7A44, Hex("8b8668010000 85c0 7519 8b4628 8b5624 8b8e98020000 03c2 3bc1 7507 b801000000 eb02 33c0"), Hex("8BCE E8")};
            Rel32(p.repl, 0x5E7A4B, Complete);
            g.Add(p);
            Patch q{0x5E7A87, Hex("8b8668010000 85c0 752e 8b4628 8b5624 8b8e98020000 03c2 3bc1 743f"), Hex("8BCE E8")};
            Rel32(q.repl, 0x5E7A8E, Complete);
            auto t = Hex("85C0 7550");
            q.repl.insert(q.repl.end(), t.begin(), t.end());
            g.Add(q);
            Patch r{0x5E7AA3, Hex("8b8668010000 85c0 7512 8b4e28 8b5624 8b8698020000 03ca 3bc8 7423"), Hex("8BCE E8")};
            Rel32(r.repl, 0x5E7AAA, Complete);
            t = Hex("85C0 7534");
            r.repl.insert(r.repl.end(), t.begin(), t.end());
            g.Add(r);
        }
        g.Add(0x5D619F, "7d57", Hex("9090"));                                  
        g.Add(Branch(0x5DB0D9, "e9b2f7ffff", 0xE9, UnloadAllTail));
        g.Add(Branch(0x5E86E0, "51 56 8bf1 e8b71af9ff", 0xE9, UpdatePositionHook));
        {
            std::vector<uint8_t> n = {0x68};
            U32(n, POOL_OBJ_SIZE + EXTRA_NODES * 0x14);
            g.Add(0x5E4E9D, "68380f0000", n);                                    // bigger pool object
        }
        g.Add(Branch(0x5E4EE0, "8b4c2404 8986a4060000", 0xE8, PoolNodes));
        g.Add(Branch(0x5E6486, "5f 8bc6 5e 5b c3", 0xE9, SceneryEntries));
        g.Apply();
    }

    // ---- 2. draw every loaded section
    if (IniInt("Render", "DrawAll", 1)) {
        IniSections("Render", "HideSections", DEFAULT_SKIP, g_state.skip, "HideSections");
        IniSections("Render", "HideInOwnDistrict", DEFAULT_HOME, g_state.home, "HideInOwnDistrict");
        Group g("DrawAll");
        g.Add(Branch(0x5E99C0, "81ec00010000", 0xE9, DrawVisibleHook));
        std::vector<uint8_t> a = {0xB8};                                         
        U32(a, (uint32_t)(uintptr_t)g_renderList);
        g.Add(0x5E9A34, "b8e06d8700", a);
        std::vector<uint8_t> b = Hex("C785CC060000");
        U32(b, (uint32_t)(uintptr_t)(g_renderList + sizeof g_renderList));
        g.Add(0x5E9A4F, "c785cc060000c0328800", b);
        g.Add(0x5D24F9, "c705f80d800001000000", Hex("c705f80d800000000000"));   // preculler off
        std::vector<uint8_t> push = {0x68}, add = {0x05}, mov = Hex("C70554DA8200");
        U32(push, FRAME_POOL_BYTES), U32(add, FRAME_POOL_BYTES), U32(mov, FRAME_POOL_BYTES);
        g.Add(0x48CD61, "6800400600", push);                                     // matrix pool malloc #1
        g.Add(0x48CD6B, "6800400600", push);                                     // malloc #2
        g.Add(0x48CD90, "0500400600", add);                                      
        g.Add(0x48CD9C, "c70554da820000400600", mov);                            // DAT_0082DA54
        if (g.Apply())
            g_state.drawAll = 1;
    }

    // ---- 3. minimum on-screen size of scenery objects (FUN_005D6570: cmp eax,imm8)
    int px = IniInt("Render", "MinObjectPixels", 11);
    if (px >= 2 && px <= 127) {
        Group g("MinObjectPixels");
        g.Add(0x5D6694, "83f811", {0x83, 0xF8, (uint8_t)px});
        g.Add(0x5D6683, "83f817", {0x83, 0xF8, (uint8_t)px});
        g.Add(0x5D6664, "83f810", {0x83, 0xF8, (uint8_t)px});
        if (g.Apply())
            Log("  %d px", px);
    } else {
        Log("[MinObjectPixels] %d: keeping the game's 17/23/16", px);
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        Install(module);
    }
    return TRUE;
}
