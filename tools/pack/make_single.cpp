// make_single.cpp — 单文件打包器/启动器二合一（零第三方依赖，MinGW 静态链接）
//
// 打包:  make_single.exe pack <dist目录> <输出exe>
//        把启动器自身 exe 作为前缀，把目录内全部文件按索引追加到末尾
// 运行时: 被打包出的 exe 双击运行（无参数）:
//        解析自身尾部索引 → 解压到 %TEMP%\my_com_run\ → 以该目录为工作目录
//        启动 my_com.exe → 等待其退出 → 清理临时文件
//
// 文件格式: [本 exe 数据][文件0][文件1]...[magic "MCSF"][count u32][indexOff u64]
// 每个文件: [nameLen u16][name(UTF-8)][size u64][data]

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct FileEntry {
    std::string name;   // 相对路径（'/' 分隔）
    std::vector<char> data;
};

std::wstring selfPath()
{
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}

std::wstring tempRunDir()
{
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    return std::wstring(tmp) + L"my_com_run";
}

bool listFiles(const std::wstring& base, const std::wstring& rel,
               std::vector<FileEntry>* out)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((base + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        const std::wstring sub = rel.empty() ? fd.cFileName
                                             : rel + L"/" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            listFiles(base + L"\\" + fd.cFileName, sub, out);
            continue;
        }
        FileEntry e;
        char name[512];
        WideCharToMultiByte(CP_UTF8, 0, sub.c_str(), -1, name, sizeof(name),
                            nullptr, nullptr);
        e.name = name;
        FILE* f = _wfopen((base + L"\\" + fd.cFileName).c_str(), L"rb");
        if (!f)
            continue;
        fseek(f, 0, SEEK_END);
        const long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        e.data.resize(sz);
        if (sz > 0)
            fread(e.data.data(), 1, sz, f);
        fclose(f);
        out->push_back(std::move(e));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
}

bool readSelf(std::vector<char>* buf)
{
    FILE* f = _wfopen(selfPath().c_str(), L"rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf->resize(sz);
    fread(buf->data(), 1, sz, f);
    fclose(f);
    return sz > 0;
}

int cmdPack(const wchar_t* distDir, const wchar_t* outExe)
{
    std::vector<FileEntry> files;
    if (!listFiles(distDir, L"", &files)) {
        std::fprintf(stderr, "无法读取目录: %ls\n", distDir);
        return 1;
    }
    std::vector<char> out;
    if (!readSelf(&out)) {
        std::fprintf(stderr, "无法读取自身 exe\n");
        return 1;
    }
    const uint64_t indexOff = out.size();
    for (const FileEntry& e : files) {
        const uint16_t nameLen = static_cast<uint16_t>(e.name.size());
        out.insert(out.end(), reinterpret_cast<const char*>(&nameLen),
                   reinterpret_cast<const char*>(&nameLen) + 2);
        out.insert(out.end(), e.name.begin(), e.name.end());
        const uint64_t size = e.data.size();
        out.insert(out.end(), reinterpret_cast<const char*>(&size),
                   reinterpret_cast<const char*>(&size) + 8);
        out.insert(out.end(), e.data.begin(), e.data.end());
    }
    const char magic[4] = {'M', 'C', 'S', 'F'};
    out.insert(out.end(), magic, magic + 4);
    const uint32_t count = static_cast<uint32_t>(files.size());
    out.insert(out.end(), reinterpret_cast<const char*>(&count),
               reinterpret_cast<const char*>(&count) + 4);
    out.insert(out.end(), reinterpret_cast<const char*>(&indexOff),
               reinterpret_cast<const char*>(&indexOff) + 8);

    FILE* f = _wfopen(outExe, L"wb");
    if (!f) {
        std::fprintf(stderr, "无法写入: %ls\n", outExe);
        return 1;
    }
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    std::printf("打包完成: %ls（%d 个文件，%.1f MB）\n", outExe,
                static_cast<int>(files.size()), out.size() / 1048576.0);
    return 0;
}

// 解压自身尾部携带的文件到 %TEMP%\my_com_run，返回内部 my_com.exe 路径。
// 文件被占用（另一实例正从该目录运行）时跳过该文件，视为已解压
bool extractSelf(std::wstring* exePathOut)
{
    // 解析尾部索引
    std::vector<char> self;
    if (!readSelf(&self) || self.size() < 16) {
        std::fprintf(stderr, "读取自身失败\n");
        return false;
    }
    const size_t off = self.size() - 8;
    uint64_t indexOff = 0;
    std::memcpy(&indexOff, self.data() + off, 8);
    if (indexOff + 16 > self.size()) {
        std::fprintf(stderr, "索引越界\n");
        return false;
    }
    // magic/count 在文件末尾，indexOff 指向文件数据起点
    const size_t tail = self.size() - 16;
    if (std::memcmp(self.data() + tail, "MCSF", 4) != 0) {
        std::fprintf(stderr, "magic 不匹配\n");
        return false;
    }
    uint32_t count = 0;
    std::memcpy(&count, self.data() + tail + 4, 4);

    // 解压到 %TEMP%\my_com_run
    const std::wstring runDir = tempRunDir();
    CreateDirectoryW(runDir.c_str(), nullptr);
    size_t pos = indexOff;
    for (uint32_t i = 0; i < count; ++i) {
        uint16_t nameLen = 0;
        std::memcpy(&nameLen, self.data() + pos, 2);
        pos += 2;
        std::string name(self.data() + pos, nameLen);
        pos += nameLen;
        uint64_t size = 0;
        std::memcpy(&size, self.data() + pos, 8);
        pos += 8;
        wchar_t wname[512];
        MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, wname, 512);
        for (wchar_t* p = wname; *p; ++p)
            if (*p == L'/')
                *p = L'\\';
        const std::wstring path = runDir + L"\\" + wname;
        if (name == "my_com.exe")
            *exePathOut = path; // 文件被占用跳过时也记录，稍后校验存在性
        // 建子目录
        for (size_t k = 0; k < name.size(); ++k) {
            if (name[k] == '/') {
                wchar_t d[512];
                MultiByteToWideChar(CP_UTF8, 0, name.c_str(), static_cast<int>(k),
                                    d, 512);
                d[k] = 0;
                CreateDirectoryW((runDir + L"\\" + d).c_str(), nullptr);
            }
        }
        FILE* f = _wfopen(path.c_str(), L"wb");
        if (!f) {
            // 另一实例可能正占用（从同一目录运行）：跳过，视为已解压
            std::fprintf(stderr, "跳过占用文件: %ls\n", path.c_str());
            pos += size;
            continue;
        }
        if (size > 0)
            fwrite(self.data() + pos, 1, static_cast<size_t>(size), f);
        fclose(f);
        pos += size;
    }

    if (exePathOut->empty() ||
        GetFileAttributesW(exePathOut->c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::fprintf(stderr, "包内未找到 my_com.exe\n");
        return false;
    }
    return true;
}

// 递归清理临时目录（先删顶层文件、收集子目录，再清空并删除各子目录）
void cleanRunDir(const std::wstring& runDir)
{
    std::vector<std::wstring> dirs;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((runDir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
                continue;
            const std::wstring p = runDir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                dirs.push_back(p);
            else
                DeleteFileW(p.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    for (const std::wstring& d : dirs) {
        WIN32_FIND_DATAW sfd;
        HANDLE sh = FindFirstFileW((d + L"\\*").c_str(), &sfd);
        if (sh != INVALID_HANDLE_VALUE) {
            do {
                if (!wcscmp(sfd.cFileName, L".") || !wcscmp(sfd.cFileName, L".."))
                    continue;
                DeleteFileW((d + L"\\" + sfd.cFileName).c_str());
            } while (FindNextFileW(sh, &sfd));
            FindClose(sh);
        }
        RemoveDirectoryW(d.c_str());
    }
    RemoveDirectoryW(runDir.c_str());
}

// 单文件 GUI 启动：解压 → 启动 my_com.exe → 等退出 → 清理
int cmdRun()
{
    FreeConsole(); // 双击运行时关闭控制台窗口
    std::wstring exePath;
    if (!extractSelf(&exePath))
        return 1;
    const std::wstring runDir = tempRunDir();
    // 启动 my_com.exe，工作目录设为解压目录（相对路径依赖正确解析）
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exePath.c_str(), nullptr, nullptr, nullptr, FALSE, 0,
                        nullptr, runDir.c_str(), &si, &pi)) {
        std::fprintf(stderr, "启动失败: %lu\n", GetLastError());
        return 1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    cleanRunDir(runDir);
    return 0;
}

std::wstring quoteArg(const std::wstring& a)
{
    if (a.find_first_of(L" \"") == std::wstring::npos)
        return a;
    std::wstring r = L"\"";
    for (wchar_t ch : a) {
        if (ch == L'"')
            r += L"\\\"";
        else
            r += ch;
    }
    r += L"\"";
    return r;
}

// 命令行模式透传：解压 → 把 cli 及后续参数原样传给内部 my_com.exe
//（stdout 直通控制台）→ 等退出 → cli.log 复制到单文件 exe 旁 → 清理。
// 退出码透传（0=成功 1=失败）
int cmdCli(int argc, wchar_t* argv[])
{
    std::wstring exePath;
    if (!extractSelf(&exePath))
        return 1;
    const std::wstring runDir = tempRunDir();
    std::wstring cmd = L"\"" + exePath + L"\"";
    for (int i = 1; i < argc; ++i)
        cmd += L" " + quoteArg(argv[i]); // 含 argv[1]="cli"，内部 exe 直接识别
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr,
                        runDir.c_str(), &si, &pi)) {
        std::fprintf(stderr, "启动失败: %lu\n", GetLastError());
        return 1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    // cli.log 复制到单文件 exe 旁（无控制台环境读文件最可靠）；失败忽略
    std::wstring exeDir = selfPath();
    const size_t slash = exeDir.find_last_of(L'\\');
    if (slash != std::wstring::npos) {
        exeDir.resize(slash + 1);
        CopyFileW((runDir + L"\\cli.log").c_str(),
                  (exeDir + L"cli.log").c_str(), FALSE);
    }
    cleanRunDir(runDir);
    return exitCode == 0 ? 0 : 1;
}

} // namespace

int wmain(int argc, wchar_t* argv[])
{
    if (argc >= 4 && !wcscmp(argv[1], L"pack"))
        return cmdPack(argv[2], argv[3]);
    if (argc >= 2 && !wcscmp(argv[1], L"cli"))
        return cmdCli(argc, argv);
    return cmdRun(); // 无参数运行 = 单文件 GUI
}
