// ============================================================================
//  wifi_list_profiles.cpp
//  ----------------------
//  Windows 11 / Visual Studio 2022 で動作確認済み
//  保存済みの Wi-Fi プロファイル一覧と、それぞれのパスワードを表示します。
//
//  実行例：
//    ---- Interface: Wi-Fi ----
//    Profile: MyHomeWiFi
//      SSID: MyHomeWiFi
//      PASS: mypassword123
//    Profile: CafeNet
//      SSID: CafeNet
//      PASS: (not available / permission denied)
//
//  ※ 管理者権限で実行すると、より多くのパスワードが取得できます。
// ============================================================================

#include <windows.h>
#include <wlanapi.h>   // Windows WLAN API
#include <iostream>    // 標準入出力
#include <string>      // std::wstring, std::string
#include <vector>
#include <algorithm>   // std::transform
#include <cwctype>     // towlower, iswspace
#pragma comment(lib, "wlanapi.lib") // WLAN API のリンク設定

// ---------------------------------------------------------------------------
//  文字列処理のユーティリティ関数群
// ---------------------------------------------------------------------------

// 前後の空白を除去する
static std::wstring trim_w(const std::wstring& s) {
    size_t a = 0;
    while (a < s.size() && iswspace(s[a])) ++a;     // 前方の空白をスキップ
    size_t b = s.size();
    while (b > a && iswspace(s[b - 1])) --b;        // 後方の空白をスキップ
    return s.substr(a, b - a);
}

// 文字列を小文字化する（大文字小文字を無視した検索などに使用）
static std::wstring tolower_w(const std::wstring& s) {
    std::wstring r = s;
    std::transform(r.begin(), r.end(), r.begin(),
        [](wchar_t c) -> wchar_t {
            // グローバル名前空間の ::towlower を使用
            return static_cast<wchar_t>(::towlower(static_cast<wint_t>(c)));
        });
    return r;
}

// XML文字列から <keyMaterial>...</keyMaterial> を抽出（Wi-Fiパスワードの場所）
static std::wstring extractKeyMaterial(const std::wstring& xml) {
    std::wstring low = tolower_w(xml); // 小文字化して検索
    const std::wstring tag1 = L"<keymaterial>";
    const std::wstring tag2 = L"</keymaterial>";

    size_t p1 = low.find(tag1);
    if (p1 == std::wstring::npos) return L""; // タグが見つからない場合
    p1 += tag1.size();
    size_t p2 = low.find(tag2, p1);
    if (p2 == std::wstring::npos) return L"";
    return trim_w(xml.substr(p1, p2 - p1));
}

// ワイド文字列を UTF-8 に変換（netsh の呼び出しなどに使用）
static std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int sz = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string out(sz, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], sz, NULL, NULL);
    return out;
}

// ---------------------------------------------------------------------------
//  フォールバック：netsh コマンドを呼び出してパスワードを抽出
// ---------------------------------------------------------------------------
//  WlanGetProfile が失敗した場合に利用。
//  出力に "Key Content"（英語）または "キー コンテンツ"（日本語）が含まれる行を解析。
// ---------------------------------------------------------------------------
static std::wstring try_netsh_parse(const std::wstring& profileName) {
    std::string profUtf8 = wide_to_utf8(profileName);
    std::string cmd = "netsh wlan show profile name=\"" + profUtf8 + "\" key=clear 2>&1";

    FILE* pipe = _popen(cmd.c_str(), "r");
    if (!pipe) return L"";

    char buf[512];
    std::wstring found;

    while (fgets(buf, sizeof(buf), pipe)) {
        std::string line(buf);
        std::string low = line;
        std::transform(low.begin(), low.end(), low.begin(),
            [](unsigned char c) { return std::tolower(c); });

        size_t pos = low.find("key content"); // 英語表記を探す
        if (pos == std::string::npos)
            pos = low.find("キー コンテンツ"); // 日本語表記（UTF-8想定）

        if (pos != std::string::npos) {
            // 「:」以降がパスワード
            size_t col = line.find(':', pos);
            if (col != std::string::npos && col + 1 < line.size()) {
                std::string val = line.substr(col + 1);
                // トリム（空白除去）
                size_t a = 0; while (a < val.size() && isspace((unsigned char)val[a])) ++a;
                size_t b = val.size(); while (b > a && isspace((unsigned char)val[b - 1])) --b;
                std::string trimmed = val.substr(a, b - a);

                // UTF-8 → wide 変換
                int wlen = MultiByteToWideChar(CP_UTF8, 0, trimmed.c_str(), (int)trimmed.size(), NULL, 0);
                if (wlen > 0) {
                    std::wstring wout(wlen, L'\0');
                    MultiByteToWideChar(CP_UTF8, 0, trimmed.c_str(), (int)trimmed.size(), &wout[0], wlen);
                    found = trim_w(wout);
                }
                break;
            }
        }
    }
    _pclose(pipe);
    return found;
}

// ---------------------------------------------------------------------------
//  メイン関数
// ---------------------------------------------------------------------------
int wmain() {
    DWORD clientVersion = 2;
    HANDLE hClient = NULL;
    DWORD negotiated = 0;

    // WLAN API ハンドルを開く
    DWORD ret = WlanOpenHandle(clientVersion, NULL, &negotiated, &hClient);
    if (ret != ERROR_SUCCESS) {
        std::wcerr << L"WlanOpenHandle failed: " << ret << L"\n";
        return 1;
    }

    // ネットワークインターフェイス一覧を取得
    PWLAN_INTERFACE_INFO_LIST pIfList = NULL;
    ret = WlanEnumInterfaces(hClient, NULL, &pIfList);
    if (ret != ERROR_SUCCESS || pIfList == NULL) {
        std::wcerr << L"WlanEnumInterfaces failed: " << ret << L"\n";
        if (pIfList) WlanFreeMemory(pIfList);
        WlanCloseHandle(hClient, NULL);
        return 2;
    }

    // 各インターフェイス（通常は「Wi-Fi」1つ）を処理
    for (unsigned int i = 0; i < pIfList->dwNumberOfItems; ++i) {
        const WLAN_INTERFACE_INFO& iface = pIfList->InterfaceInfo[i];
        std::wstring ifaceName = iface.strInterfaceDescription;

        std::wcout << L"---- Interface: " << ifaceName << L" ----\n";

        // このインターフェイスのプロファイル一覧を取得
        PWLAN_PROFILE_INFO_LIST pProfileList = NULL;
        ret = WlanGetProfileList(hClient, &iface.InterfaceGuid, NULL, &pProfileList);
        if (ret != ERROR_SUCCESS || pProfileList == NULL) {
            std::wcerr << L"  WlanGetProfileList failed: " << ret << L"\n";
            continue;
        }

        // プロファイルを1件ずつ処理
        for (unsigned int j = 0; j < pProfileList->dwNumberOfItems; ++j) {
            WLAN_PROFILE_INFO pinfo = pProfileList->ProfileInfo[j];
            std::wstring profileName = pinfo.strProfileName; // SSID とほぼ同義
            std::wcout << L"Profile: " << profileName << L"\n";

            LPWSTR pProfileXml = NULL;
            DWORD dwFlags = WLAN_PROFILE_GET_PLAINTEXT_KEY; // 平文のキーを取得したい
            DWORD grantedAccess = 0;

            // プロファイルの XML データを取得
            ret = WlanGetProfile(hClient, &iface.InterfaceGuid,
                profileName.c_str(),
                NULL,
                &pProfileXml,
                &dwFlags,
                &grantedAccess);

            std::wstring password;

            if (ret == ERROR_SUCCESS && pProfileXml != NULL) {
                // XML から <keyMaterial>...</keyMaterial> を抽出
                std::wstring xml = pProfileXml;
                password = extractKeyMaterial(xml);
                WlanFreeMemory(pProfileXml);
            }

            // WlanGetProfile で取得できなかった場合、netsh にフォールバック
            if (password.empty()) {
                password = try_netsh_parse(profileName);
            }

            // 結果を出力
            std::wcout << L"  SSID: " << profileName << L"\n";
            if (!password.empty()) {
                std::wcout << L"  PASS: " << password << L"\n";
            }
            else {
                std::wcout << L"  PASS: (not available / permission denied)\n";
            }
        }

        // メモリを解放
        if (pProfileList) WlanFreeMemory(pProfileList);
    }

    // 最後に後片付け
    if (pIfList) WlanFreeMemory(pIfList);
    WlanCloseHandle(hClient, NULL);
    system("pause");
    return 0;
}
