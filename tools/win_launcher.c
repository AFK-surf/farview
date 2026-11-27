#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int detect_windows_dark(void) {
  HKEY hkey = NULL;
  DWORD val = 1;
  DWORD sz = sizeof(val);
  DWORD type = 0;
  if (RegOpenKeyExA(HKEY_CURRENT_USER,
                    "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                    0, KEY_READ, &hkey) == ERROR_SUCCESS) {
    if (RegQueryValueExA(hkey, "AppsUseLightTheme", NULL, &type, (LPBYTE)&val, &sz) == ERROR_SUCCESS &&
        type == REG_DWORD) {
      RegCloseKey(hkey);
      return val == 0 ? 1 : 0;
    }
    RegCloseKey(hkey);
  }
  return 0;
}

static int resolve_prefers_dark(const char *profile_dir) {
  char settings_path[MAX_PATH];
  snprintf(settings_path, sizeof(settings_path), "%s\\settings.json", profile_dir);
  FILE *f = fopen(settings_path, "rb");
  if (f) {
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    const char *k = strstr(buf, "\"chrome_theme\"");
    if (k) {
      const char *colon = strchr(k + 14, ':');
      if (colon) {
        while (*colon && (*colon == ':' || *colon == ' ' || *colon == '\t' || *colon == '"')) colon++;
        if (strncmp(colon, "dark", 4) == 0) return 1;
        if (strncmp(colon, "light", 5) == 0) return 0;
      }
    }
  }
  return detect_windows_dark();
}

static int read_host_json_port(const char *host_json_path) {
  FILE *f = fopen(host_json_path, "rb");
  if (!f) return 0;
  char buf[512];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = '\0';
  const char *p = strstr(buf, "\"port\"");
  if (!p) return 0;
  p = strchr(p + 6, ':');
  if (!p) return 0;
  p++;
  while (*p == ' ' || *p == '\t') p++;
  int port = atoi(p);
  return (port > 0 && port <= 65535) ? port : 0;
}

static void strip_outer_quotes(const char *in, char *out, size_t cap) {
  while (*in == ' ' || *in == '\t') in++;
  size_t len = strlen(in);
  while (len > 0 && (in[len - 1] == ' ' || in[len - 1] == '\t')) len--;
  if (len >= 2 && in[0] == '"' && in[len - 1] == '"') {
    in++;
    len -= 2;
  }
  if (len >= cap) len = cap - 1;
  memcpy(out, in, len);
  out[len] = '\0';
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
  (void)hInst; (void)hPrev; (void)nShow;

  char exe[MAX_PATH];
  DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return 1;
  char *slash = strrchr(exe, '\\');
  if (slash) *slash = '\0';

  char url[1024];
  if (lpCmd && lpCmd[0]) {
    strip_outer_quotes(lpCmd, url, sizeof(url));
  } else {
    url[0] = '\0';
  }
  if (!url[0]) {
    snprintf(url, sizeof(url), "about:newtab");
  }

  char state_dir[MAX_PATH];
  DWORD env_state = GetEnvironmentVariableA("FV_STATE_DIR", state_dir, sizeof(state_dir));
  if (env_state == 0 || env_state >= sizeof(state_dir)) {
    char local_app[MAX_PATH];
    DWORD la = GetEnvironmentVariableA("LOCALAPPDATA", local_app, sizeof(local_app));
    if (la > 0 && la < sizeof(local_app) - 16) {
      snprintf(state_dir, sizeof(state_dir), "%s\\Farview", local_app);
    } else {
      snprintf(state_dir, sizeof(state_dir), "%s\\state", exe);
    }
  }
  CreateDirectoryA(state_dir, NULL);

  char profile_dir[MAX_PATH];
  DWORD env_prof = GetEnvironmentVariableA("FV_PROFILE_DIR", profile_dir, sizeof(profile_dir));
  if (env_prof == 0 || env_prof >= sizeof(profile_dir)) {
    snprintf(profile_dir, sizeof(profile_dir), "%s", state_dir);
  }
  CreateDirectoryA(profile_dir, NULL);

  char host_bin[MAX_PATH];
  snprintf(host_bin, sizeof(host_bin), "%s\\pion-host.exe", exe);
  char host_json[MAX_PATH];
  snprintf(host_json, sizeof(host_json), "%s\\host.json", state_dir);
  DeleteFileA(host_json);

  int dark = resolve_prefers_dark(profile_dir);
  SetEnvironmentVariableA("FV_STATE_DIR", state_dir);
  SetEnvironmentVariableA("FV_PROFILE_DIR", profile_dir);
  SetEnvironmentVariableA("FV_HOST_BIN", host_bin);
  SetEnvironmentVariableA("FV_NEW_TABS", "1");
  SetEnvironmentVariableA("FV_PREFERS_DARK", dark ? "1" : "0");
  char fps_env[32];
  if (GetEnvironmentVariableA("MBUI_FRAMES_MOST", fps_env, sizeof(fps_env)) == 0) {
    SetEnvironmentVariableA("MBUI_FRAMES_MOST", "240");
  }

  char host_log[MAX_PATH];
  snprintf(host_log, sizeof(host_log), "%s\\host.log", state_dir);
  SECURITY_ATTRIBUTES sa;
  memset(&sa, 0, sizeof(sa));
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  HANDLE h_log = CreateFileA(host_log, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

  char host_cmd[2048];
  snprintf(host_cmd, sizeof(host_cmd), "\"%s\" --tab 0 \"%s\"", host_bin, url);
  STARTUPINFOA si;
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  if (h_log != INVALID_HANDLE_VALUE) {
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = h_log;
    si.hStdError = h_log;
  }
  PROCESS_INFORMATION host_pi;
  memset(&host_pi, 0, sizeof(host_pi));
  if (!CreateProcessA(NULL, host_cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, exe, &si, &host_pi)) {
    if (h_log != INVALID_HANDLE_VALUE) CloseHandle(h_log);
    MessageBoxA(NULL, "Could not start pion-host.exe.", "Farview", MB_OK | MB_ICONERROR);
    return 1;
  }
  if (h_log != INVALID_HANDLE_VALUE) CloseHandle(h_log);

  int port = 0;
  for (int i = 0; i < 150; i++) {
    port = read_host_json_port(host_json);
    if (port > 0) break;
    if (WaitForSingleObject(host_pi.hProcess, 0) != WAIT_TIMEOUT) break;
    Sleep(100);
  }
  if (port <= 0) {
    TerminateProcess(host_pi.hProcess, 1);
    CloseHandle(host_pi.hProcess);
    CloseHandle(host_pi.hThread);
    char msg[512];
    snprintf(msg, sizeof(msg), "The page host did not start.\nLog: %s", host_log);
    MessageBoxA(NULL, msg, "Farview", MB_OK | MB_ICONERROR);
    return 1;
  }

  char port_str[16];
  snprintf(port_str, sizeof(port_str), "%d", port);
  SetEnvironmentVariableA("FV_APP_PORT", port_str);

  char app_cmd[1024];
  snprintf(app_cmd, sizeof(app_cmd), "\"%s\\pion-app.exe\"", exe);
  STARTUPINFOA asi;
  memset(&asi, 0, sizeof(asi));
  asi.cb = sizeof(asi);
  asi.lpDesktop = "WinSta0\\Default";
  PROCESS_INFORMATION app_pi;
  memset(&app_pi, 0, sizeof(app_pi));
  if (CreateProcessA(NULL, app_cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, exe, &asi, &app_pi)) {
    WaitForSingleObject(app_pi.hProcess, INFINITE);
    CloseHandle(app_pi.hProcess);
    CloseHandle(app_pi.hThread);
  }

  TerminateProcess(host_pi.hProcess, 0);
  CloseHandle(host_pi.hProcess);
  CloseHandle(host_pi.hThread);
  return 0;
}
