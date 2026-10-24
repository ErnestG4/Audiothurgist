#pragma once
// The FatFs surface the firmware's card code touches, over stdio / POSIX, for
// the host suites (persist_check: the WAV loader; card_check: core/bb_cardfs.h).
// A path that starts "0:" is the card: it maps under gShimCard (a scratch
// directory the test sets); anything else is used as it is (persist_check
// hands the loader real paths). FatFs's own rules where they differ from
// POSIX: f_rename never replaces (FR_EXIST), f_mkdir of an existing folder is
// FR_EXIST, a short write with FR_OK is "disk full". Not emulated: FAT's case
// insensitivity (the tests keep one spelling) and 8.3 names.
//
// Fault and law hooks: gShimFree bytes the card may still take (a write past it
// is cut short, FatFs's full card); gShimWrites counts f_write calls and
// gShimMisaligned those whose whole-sector part would start off a word boundary
// (the SDMMC IDMA rule, sd_card.h's rule 3 — the staging must be offset by
// DmaPad(file offset)).
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#define DIR PosixDir_   /* POSIX's DIR would collide with FatFs's */
#include <dirent.h>
#undef DIR

typedef unsigned UINT;
enum FRESULT { FR_OK = 0, FR_DISK_ERR = 1, FR_INT_ERR = 2, FR_NOT_READY = 3, FR_NO_FILE = 4, FR_NO_PATH = 5,
               FR_INVALID_NAME = 6, FR_DENIED = 7, FR_EXIST = 8 };
#define FA_READ          0x01
#define FA_WRITE         0x02
#define FA_CREATE_NEW    0x04
#define FA_CREATE_ALWAYS 0x08
#define AM_DIR           0x10
struct FIL { FILE* fp; uint32_t pos; };
struct DIR { PosixDir_* d; std::string path; };
struct FILINFO { uint32_t fsize; uint8_t fattrib; char fname[256]; };

static std::string gShimCard;                 // the card's directory on the host
static long long   gShimFree = -1;            // bytes the card still takes; < 0 = unlimited
static int         gShimWrites = 0, gShimMisaligned = 0;

static std::string ShimPath(const char* p)
{
    if(p[0] == '0' && p[1] == ':')
    {
        const char* r = p + 2;
        while(*r == '/') r++;
        return *r ? gShimCard + "/" + r : gShimCard;
    }
    return p;
}
static bool ShimExists(const std::string& p, bool* isDir = nullptr)
{
    struct stat sb;
    if(stat(p.c_str(), &sb) != 0) return false;
    if(isDir) *isDir = S_ISDIR(sb.st_mode);
    return true;
}
static FRESULT f_open(FIL* f, const char* n, int mode)
{
    const std::string p = ShimPath(n);
    f->pos = 0;
    if(mode & FA_WRITE)
    {
        if((mode & FA_CREATE_NEW) && ShimExists(p)) return FR_EXIST;
        f->fp = fopen(p.c_str(), "wb");
        return f->fp ? FR_OK : FR_NO_PATH;
    }
    f->fp = fopen(p.c_str(), "rb");
    return f->fp ? FR_OK : FR_NO_FILE;
}
static FRESULT f_read(FIL* f, void* b, UINT n, UINT* br) { *br = (UINT)fread(b, 1, n, f->fp); f->pos += *br; return FR_OK; }
static FRESULT f_write(FIL* f, const void* b, UINT n, UINT* bw)
{
    gShimWrites++;
    const uint32_t head = (512u - (f->pos % 512u)) % 512u;
    if(n > head && (((uintptr_t)b + head) & 3u)) gShimMisaligned++;
    UINT k = n;
    if(gShimFree >= 0 && (long long)k > gShimFree) k = (UINT)gShimFree;
    *bw = (UINT)fwrite(b, 1, k, f->fp);
    if(gShimFree >= 0) gShimFree -= *bw;
    f->pos += *bw;
    return FR_OK;
}
static FRESULT f_lseek(FIL* f, uint32_t o) { if(fseek(f->fp, (long)o, SEEK_SET) != 0) return FR_DISK_ERR; f->pos = o; return FR_OK; }
static uint32_t f_tell(FIL* f) { return (uint32_t)ftell(f->fp); }
static FRESULT f_close(FIL* f) { const int r = fclose(f->fp); f->fp = nullptr; return r == 0 ? FR_OK : FR_DISK_ERR; }
static FRESULT f_opendir(DIR* d, const char* n)
{
    d->path = ShimPath(n);
    d->d = opendir(d->path.c_str());
    return d->d ? FR_OK : FR_NO_PATH;
}
static FRESULT f_readdir(DIR* d, FILINFO* fno)
{
    for(;;)
    {
        struct dirent* e = readdir(d->d);
        if(!e) { fno->fname[0] = 0; return FR_OK; }
        if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(fno->fname, sizeof fno->fname, "%s", e->d_name);
        struct stat sb;
        const std::string full = d->path + "/" + e->d_name;
        fno->fattrib = (stat(full.c_str(), &sb) == 0 && S_ISDIR(sb.st_mode)) ? AM_DIR : 0;
        fno->fsize   = fno->fattrib ? 0u : (uint32_t)sb.st_size;
        return FR_OK;
    }
}
static FRESULT f_closedir(DIR* d) { if(d->d) closedir(d->d); d->d = nullptr; return FR_OK; }
static FRESULT f_stat(const char* n, FILINFO* fno)
{
    const std::string p = ShimPath(n);
    struct stat sb;
    if(stat(p.c_str(), &sb) != 0) return FR_NO_FILE;
    fno->fattrib = S_ISDIR(sb.st_mode) ? AM_DIR : 0;
    fno->fsize   = (uint32_t)sb.st_size;
    const char* b = strrchr(p.c_str(), '/');
    snprintf(fno->fname, sizeof fno->fname, "%s", b ? b + 1 : p.c_str());
    return FR_OK;
}
static FRESULT f_mkdir(const char* n)
{
    const std::string p = ShimPath(n);
    if(ShimExists(p)) return FR_EXIST;
    return mkdir(p.c_str(), 0755) == 0 ? FR_OK : FR_NO_PATH;
}
static FRESULT f_unlink(const char* n) { return remove(ShimPath(n).c_str()) == 0 ? FR_OK : FR_NO_FILE; }
static FRESULT f_rename(const char* a, const char* b)
{
    const std::string pa = ShimPath(a), pb = ShimPath(b);
    if(!ShimExists(pa)) return FR_NO_FILE;
    if(ShimExists(pb)) return FR_EXIST;
    return rename(pa.c_str(), pb.c_str()) == 0 ? FR_OK : FR_DENIED;
}
