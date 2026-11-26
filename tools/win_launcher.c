#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int port_open(int port) {
  SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
  if (s == INVALID_SOCKET) return 0;
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons((u_short)port);
  int ok = (connect(s, (struct sockaddr *)&a, sizeof(a)) == 0);
  closesocket(s);
  return ok;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
  (void)hInst; (void)hPrev; (void)nShow;
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);

  char exe[MAX_PATH];
  DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return 1;
  char *slash = strrchr(exe, '\\');
  if (slash) *slash = '\0';

  const char *url = (lpCmd && lpCmd[0]) ? lpCmd : "https://example.com";
  char host_cmd[1024];
  char app_cmd[1024];
  char state_dir[MAX_PATH];
  snprintf(state_dir, sizeof(state_dir), "%s\\state", exe);
  CreateDirectoryA(state_dir, NULL);
  SetEnvironmentVariableA("FV_STATE_DIR", state_dir);
  SetEnvironmentVariableA("FV_PROFILE_DIR", state_dir);
  SetEnvironmentVariableA("FV_APP_PORT", "7070");

  PROCESS_INFORMATION host_pi;
  memset(&host_pi, 0, sizeof(host_pi));
  int started_host = 0;

  if (!port_open(7070)) {
    snprintf(host_cmd, sizeof(host_cmd), "\"%s\\pion-host.exe\" --tab 7070 \"%s\"", exe, url);
    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (CreateProcessA(NULL, host_cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, exe, &si, &host_pi)) {
      started_host = 1;
      for (int i = 0; i < 60; i++) {
        if (port_open(7070)) break;
        Sleep(100);
      }
    }
  }

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

  if (started_host) {
    TerminateProcess(host_pi.hProcess, 0);
    CloseHandle(host_pi.hProcess);
    CloseHandle(host_pi.hThread);
  }
  WSACleanup();
  return 0;
}
