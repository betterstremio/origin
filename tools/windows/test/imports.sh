#!/bin/bash
# What each shipped file needs from Windows: its imported DLLs, and any that a fresh Windows 10/11 does not have
# (a GPU driver's or a Visual C++ runtime's would fail to load on some PCs). From Git Bash it hands itself to WSL.
#   tools/windows/test/imports.sh   (after packaging/windows/pack.sh)
HERE=$(cd "$(dirname "$0")/../../.." && pwd)
if [ -z "${WSL_DISTRO_NAME:-}" ]; then exec env MSYS_NO_PATHCONV=1 wsl -u root bash "/mnt/c${HERE#/c}/tools/windows/test/imports.sh"; fi
P=/root/obuild/windows/windows/pkg/Origin
# in every Windows 10/11 install (System32), or shipped beside the exe
BASE=" kernel32.dll user32.dll gdi32.dll advapi32.dll shell32.dll ole32.dll oleaut32.dll ws2_32.dll iphlpapi.dll crypt32.dll bcrypt.dll setupapi.dll winmm.dll imm32.dll version.dll psapi.dll dwmapi.dll shlwapi.dll msvcrt.dll ntdll.dll comdlg32.dll dxgi.dll d3d11.dll d3d9.dll d3dcompiler_47.dll dwrite.dll avrt.dll mfplat.dll mf.dll mfreadwrite.dll secur32.dll ncrypt.dll normaliz.dll wldap32.dll dbghelp.dll userenv.dll mswsock.dll cfgmgr32.dll hid.dll uxtheme.dll powrprof.dll mmdevapi.dll ksuser.dll propsys.dll winhttp.dll wininet.dll iertutil.dll urlmon.dll dnsapi.dll rpcrt4.dll dcomp.dll dxva2.dll evr.dll mfuuid.dll strmiids.dll uuid.dll opengl32.dll glu32.dll avicap32.dll bcryptprimitives.dll d2d1.dll shcore.dll cfgmgr32.dll "
SHIP=" libegl.dll libglesv2.dll libmpv-2.dll libsbengine.dll vulkan-1.dll "   # (vulkan-1.dll: the PC's, else fallback/)
for f in Origin.exe libEGL.dll libGLESv2.dll libmpv-2.dll libsbengine.dll fallback/vulkan-1.dll; do
  imp=$(x86_64-w64-mingw32-objdump -p "$P/$f" | sed -n 's/.*DLL Name: //p' | tr 'A-Z' 'a-z' | sort -u)
  odd=""; for d in $imp; do case "$d" in api-ms-win-*) continue;; esac; case "$BASE$SHIP" in *" $d "*) ;; *) odd="$odd $d";; esac; done
  printf '%-16s %2d DLLs%s\n' "$f" "$(echo "$imp" | wc -w)" "${odd:+   NOT IN WINDOWS:$odd}"
done
