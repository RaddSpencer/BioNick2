// Where the host keeps its files. Next to the exe when the exe sits beside its assets folder (a release layout), otherwise in the folder above the exe's
// (the development layout: host\build\bcr2.exe with host\assets, host\saves, host\bcr2.ini, host\game ...).
#pragma once
#include <string>
std::string bcr2_data_root();
