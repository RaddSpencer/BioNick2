// First-run setup. When the game has not been unpacked yet, asks the player for their copy of the Bionic Commando Rearmed 2 package, checks it against the
// known checksum, unpacks it beside the host (host\game) and offers to start. Later runs find the unpacked game and skip all of this.
#include "firstrun.h"
#include "extract.h"
#include "paths.h"
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <commdlg.h>
#include <string>
#include <cstdio>

namespace {

const char* const kTitle = "Bio Nick 2 - First-time setup";

std::string exe_dir() {
    char p[MAX_PATH]; GetModuleFileNameA(nullptr, p, MAX_PATH);
    std::string s = p; const size_t k = s.find_last_of("\\/");
    return k == std::string::npos ? "." : s.substr(0, k);
}

bool exists(const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::wstring wide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0'); MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), &w[0], n); return w;
}

void remove_tree(const std::string& dir) {
    if (!exists(dir)) return;
    std::string from = dir; from.push_back('\0');   // SHFileOperation wants a double-terminated list
    SHFILEOPSTRUCTA op{}; op.wFunc = FO_DELETE; op.pFrom = from.c_str(); op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    SHFileOperationA(&op);
}

// The standard Windows progress dialog, with Cancel.
struct Progress {
    IProgressDialog* d = nullptr;
    Progress(const char* title, const char* line1) {
        if (SUCCEEDED(CoCreateInstance(CLSID_ProgressDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) {
            d->SetTitle(wide(title).c_str());
            d->SetLine(1, wide(line1).c_str(), FALSE, nullptr);
            d->StartProgressDialog(nullptr, nullptr, PROGDLG_NORMAL | PROGDLG_AUTOTIME, nullptr);
        }
    }
    ~Progress() { if (d) { d->StopProgressDialog(); d->Release(); } }
    bool update(unsigned long long done, unsigned long long total, const std::string& line2) {
        if (!d) return true;
        d->SetProgress64(done, total ? total : 1);
        d->SetLine(2, wide(line2).c_str(), TRUE, nullptr);
        return !d->HasUserCancelled();
    }
};

bool browse(std::string& path) {
    char file[MAX_PATH * 2] = {};
    OPENFILENAMEA o{}; o.lStructSize = sizeof(o);
    o.lpstrFilter = "All files (*.*)\0*.*\0";
    o.lpstrFile = file; o.nMaxFile = sizeof(file);
    o.lpstrTitle = "Select your Bionic Commando Rearmed 2 (XBLA, World) file";
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
    if (!GetOpenFileNameA(&o)) return false;
    path = file; return true;
}

}  // namespace

bool game_is_unpacked(const std::string& gameDir) {
    return exists(gameDir + "\\basefile.bin") && exists(gameDir + "\\extracted_out\\assets\\all.bundle") && exists(gameDir + "\\.complete");
}

std::string game_dir() { return bcr2_data_root() + "\\game"; }

bool first_run_setup(std::string& basefile, std::string& contentRoot) {
    const std::string dir = game_dir();
    basefile = dir + "\\basefile.bin"; contentRoot = dir + "\\extracted_out";
    if (game_is_unpacked(dir)) return true;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    struct CoGuard { ~CoGuard() { CoUninitialize(); } } coGuard;

    char notice[2048];
    snprintf(notice, sizeof(notice),
             "Bio Nick 2 requires the XBLA \"World\" release of Bionic Commando Rearmed 2. You will need to dump this yourself. Please browse for your copy of the file "
             "(most likely named \"52F33688A9A36ABD078C31F43611F166352655B958\" with no extension).\n\n"
             "The file this version needs:\n"
             "  Size: about 1.41 GB\n"
             "  SHA-256: %s\n"
             "  SHA-1: %s\n"
             "  MD5: %s\n\n"
             "It is checked before anything is unpacked.", kPackageSha256, kPackageSha1, kPackageMd5);
    if (MessageBoxA(nullptr, notice, kTitle, MB_OKCANCEL | MB_ICONINFORMATION | MB_SETFOREGROUND) != IDOK) return false;

    for (;;) {
        std::string pkg;
        if (!browse(pkg)) return false;

        std::string sha; bool cancelled = false;
        {
            Progress p(kTitle, "Checking the file...");
            sha = file_sha256(pkg, [&](unsigned long long d, unsigned long long t, const std::string&) { bool ok = p.update(d, t, pkg); if (!ok) cancelled = true; return ok; });
        }
        if (cancelled) return false;
        if (sha != kPackageSha256) {
            char m[2048];
            if (sha.empty())
                snprintf(m, sizeof(m), "That file could not be read:\n%s", pkg.c_str());
            else
                snprintf(m, sizeof(m), "That is not the file this version needs.\n\nSHA-256 of the file you chose:\n  %s\n\nSHA-256 needed:\n  %s\n\nIt must be the XBLA \"World\" release of Bionic Commando Rearmed 2. Choose another file?", sha.c_str(), kPackageSha256);
            if (MessageBoxA(nullptr, m, kTitle, MB_RETRYCANCEL | MB_ICONWARNING | MB_SETFOREGROUND) == IDRETRY) continue;
            return false;
        }

        // Unpack beside the final folder and move it into place at the end, so an interrupted run leaves nothing half done.
        const std::string tmp = dir + ".tmp";
        remove_tree(tmp); remove_tree(dir);
        ExtractResult r;
        cancelled = false;
        {
            Progress p(kTitle, "Unpacking the game (this takes a minute)...");
            r = extract_package(pkg, tmp, [&](unsigned long long d, unsigned long long t, const std::string& what) { bool ok = p.update(d, t, what); if (!ok) cancelled = true; return ok; });
        }
        if (!r.ok) {
            remove_tree(tmp);
            if (cancelled) return false;
            std::string m = "Unpacking failed:\n" + r.error + "\n\nTry again?";
            if (MessageBoxA(nullptr, m.c_str(), kTitle, MB_RETRYCANCEL | MB_ICONERROR | MB_SETFOREGROUND) == IDRETRY) continue;
            return false;
        }
        if (FILE* f = fopen((tmp + "\\.complete").c_str(), "w")) { fputs("ok\n", f); fclose(f); }
        if (!MoveFileExA(tmp.c_str(), dir.c_str(), 0)) {
            remove_tree(tmp);
            MessageBoxA(nullptr, "Could not put the unpacked game in place.", kTitle, MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
            return false;
        }
        if (!r.baseKnown)
            MessageBoxA(nullptr, "The game was unpacked, but its program image is not the one this version of Bio Nick 2 was built from. It may not run correctly.", kTitle, MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
        return MessageBoxA(nullptr, "Setup is complete.\n\nOpen Bio Nick 2 now?\n\nThe first start compiles the game's shaders, which takes a little while.", kTitle, MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) == IDYES;
    }
}
