// First-time setup: unpacks the game from an Xbox 360 content package (the file Xbox Live Arcade games are distributed in, STFS "LIVE"/"PIRS"/"CON ").
//
//   <out>\extracted_out\...   every file of the package, as the game sees them on its D: drive (assets, game.ver, dieselx.cfg, default.xex, ...)
//   <out>\basefile.bin        the game's executable image: default.xex decrypted and decompressed
//
// This is all the host needs to run the game, together with the recompiled code built into it.
#pragma once
#include <string>
#include <functional>

struct ExtractResult {
    bool ok = false;
    std::string error;           // set when !ok
    std::string baseSha256;      // SHA-256 of basefile.bin, lowercase hex
    bool baseKnown = false;      // the image is the one the recompiled code was made from
    unsigned long long files = 0, bytes = 0;
};

// progress(done_bytes, total_bytes, current_file) is called now and then; it may be empty.
ExtractResult extract_package(const std::string& package, const std::string& outDir,
                              const std::function<void(unsigned long long, unsigned long long, const std::string&)>& progress);

// The "bcr2 --extract <package> <outdir>" command line.
int extract_main(int argc, char** argv);
