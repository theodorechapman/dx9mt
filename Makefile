WINE := $(CURDIR)/Wine Staging.app/Contents/Resources/wine/bin/wine
WINESERVER := $(CURDIR)/Wine Staging.app/Contents/Resources/wine/bin/wineserver
WINEPREFIX := $(CURDIR)/wineprefix
WINE_SILENT := WINEDEBUG=-all "$(WINE)"

DX9MT_DIR := $(CURDIR)/dx9mt
DX9MT_DLL := $(DX9MT_DIR)/build/d3d9.dll
FNV_DIR := $(WINEPREFIX)/drive_c/Games/Steam/steamapps/common/Fallout New Vegas

STEAM_WIN_PATH := C:\\Games\\Steam\\steam.exe
LAUNCHER_WIN_PATH := C:\\Games\\Steam\\steamapps\\common\\Fallout New Vegas\\FalloutNVLauncher.exe

STEAM_STARTUP_WAIT ?= 12
DX9MT_RUNTIME_LOG ?= /tmp/dx9mt_runtime.log
DX9MT_LAUNCHER_LOG ?= /tmp/fnv_dx9mt_probe.log
DX9MT_STEAM_LOG ?= /tmp/steam_probe.log
DX9MT_ANALYZE_TOP ?= 20

WINEDLLOVERRIDES := dxsetup.exe=d

export WINEPREFIX
export WINEDLLOVERRIDES

.DEFAULT_GOAL := run
.PHONY: run show-logs analyze-logs test clear dx9mt-build dx9mt-test install-dx9mt-fnv configure-fnv-dx9mt-override show-fnv-dx9mt-override wine-restart

wine-restart:
	@echo "Restarting wineserver"; \
	"$(WINESERVER)" --kill >/dev/null 2>&1 || true; \
	sleep 1; \
	"$(WINESERVER)" -p >/dev/null 2>&1 || true

dx9mt-build:
	@$(MAKE) --no-print-directory -s -C "$(DX9MT_DIR)"

dx9mt-test:
	@$(MAKE) --no-print-directory -s -C "$(DX9MT_DIR)" test-native

test: dx9mt-test

configure-fnv-dx9mt-override: wine-restart
	@set -e; \
	for dll in d3d8 d3d9 d3d10 d3d10_1 d3d10core d3d11 d3d12 dxgi; do \
		$(WINE_SILENT) reg delete "HKCU\Software\Wine\DllOverrides" /v $$dll /f >/dev/null 2>&1 || true; \
		$(WINE_SILENT) reg delete "HKCU\Software\Wine\AppDefaults\steam.exe\DllOverrides" /v $$dll /f >/dev/null 2>&1 || true; \
		$(WINE_SILENT) reg delete "HKCU\Software\Wine\AppDefaults\FalloutNVLauncher.exe\DllOverrides" /v $$dll /f >/dev/null 2>&1 || true; \
		$(WINE_SILENT) reg delete "HKCU\Software\Wine\AppDefaults\FalloutNV.exe\DllOverrides" /v $$dll /f >/dev/null 2>&1 || true; \
	done; \
	$(WINE_SILENT) reg add "HKCU\Software\Wine\AppDefaults\FalloutNV.exe\DllOverrides" /v d3d9 /d native,builtin /f >/dev/null; \
	$(WINE_SILENT) reg add "HKCU\Software\Wine\AppDefaults\FalloutNVLauncher.exe\DllOverrides" /v d3d9 /d native,builtin /f >/dev/null

install-dx9mt-fnv: dx9mt-build configure-fnv-dx9mt-override
	@test -f "$(DX9MT_DLL)" || (echo "missing $(DX9MT_DLL)" && exit 1)
	@test -d "$(FNV_DIR)" || (echo "missing Fallout New Vegas dir: $(FNV_DIR)" && exit 1)
	@install -m 0644 "$(DX9MT_DLL)" "$(FNV_DIR)/d3d9.dll"

DX9MT_METAL_IPC_FILE := /tmp/dx9mt_metal_frame.bin
DX9MT_METAL_VIEWER := $(DX9MT_DIR)/build/dx9mt_metal_viewer

run: install-dx9mt-fnv
	@set -e; \
	pkill -f dx9mt_metal_viewer 2>/dev/null || true; \
	: > "$(DX9MT_RUNTIME_LOG)"; \
	: > "$(DX9MT_LAUNCHER_LOG)"; \
	: > "$(DX9MT_STEAM_LOG)"; \
	echo "Creating Metal IPC shared file"; \
	dd if=/dev/zero of="$(DX9MT_METAL_IPC_FILE)" bs=1048576 count=16 >/dev/null 2>&1; \
	if [ -x "$(DX9MT_METAL_VIEWER)" ]; then \
		echo "Launching Metal viewer"; \
		"$(DX9MT_METAL_VIEWER)" &  \
		VIEWER_PID=$$!; \
		echo "Metal viewer pid $$VIEWER_PID"; \
		sleep 1; \
	else \
		echo "Metal viewer not found at $(DX9MT_METAL_VIEWER), skipping"; \
	fi; \
	echo "Starting wineserver"; \
	"$(WINESERVER)" -p >/dev/null 2>&1 || true; \
	echo "Starting Steam (log: $(DX9MT_STEAM_LOG))"; \
	DX9MT_LOG_PATH="$(DX9MT_RUNTIME_LOG)" WINEDLLOVERRIDES="$(WINEDLLOVERRIDES)" "$(WINE)" "$(STEAM_WIN_PATH)" >>"$(DX9MT_STEAM_LOG)" 2>&1 & \
	STEAM_PID=$$!; \
	echo "Steam pid $$STEAM_PID, waiting $(STEAM_STARTUP_WAIT)s"; \
	sleep "$(STEAM_STARTUP_WAIT)"; \
	if ! kill -0 $$STEAM_PID 2>/dev/null; then \
		echo "Steam exited before launcher start. Check $(DX9MT_STEAM_LOG)."; \
		exit 1; \
	fi; \
	echo "Launching FalloutNVLauncher.exe (log: $(DX9MT_LAUNCHER_LOG))"; \
	echo "dx9mt runtime log: $(DX9MT_RUNTIME_LOG)"; \
	DX9MT_LOG_PATH="$(DX9MT_RUNTIME_LOG)" WINEDLLOVERRIDES="$(WINEDLLOVERRIDES)" "$(WINE)" "$(LAUNCHER_WIN_PATH)" >>"$(DX9MT_LAUNCHER_LOG)" 2>&1 & \
	LAUNCHER_PID=$$!; \
	echo "Launcher command pid $$LAUNCHER_PID"; \
	sleep 2; \
	if kill -0 $$LAUNCHER_PID 2>/dev/null; then \
		echo "Launcher command still running."; \
	else \
		echo "Launcher command returned quickly (this can be normal if it hands off)."; \
	fi; \
	echo "Run complete. Use 'make show-logs' after reproducing behavior."

show-logs:
	@echo "== dx9mt runtime log: $(DX9MT_RUNTIME_LOG) =="; \
	if [ -f "$(DX9MT_RUNTIME_LOG)" ]; then \
		stat -f "%Sm" -t "%Y-%m-%d %H:%M:%S" "$(DX9MT_RUNTIME_LOG)"; \
		if [ -s "$(DX9MT_RUNTIME_LOG)" ]; then \
			cat "$(DX9MT_RUNTIME_LOG)"; \
		else \
			echo "(runtime log exists but is empty)"; \
		fi; \
	else \
		echo "(no runtime log yet)"; \
	fi; \
	echo; \
	echo "== launcher signals: $(DX9MT_LAUNCHER_LOG) =="; \
	if [ -f "$(DX9MT_LAUNCHER_LOG)" ]; then \
		stat -f "%Sm" -t "%Y-%m-%d %H:%M:%S" "$(DX9MT_LAUNCHER_LOG)"; \
		rg -n -i "dx9mt/|Unhandled exception|c000|err:|D3DERR|Direct3DCreate9|CreateDevice" "$(DX9MT_LAUNCHER_LOG)" || tail -n 120 "$(DX9MT_LAUNCHER_LOG)"; \
	else \
		echo "(no launcher log yet)"; \
	fi; \
	echo; \
	echo "== steam signals: $(DX9MT_STEAM_LOG) =="; \
	if [ -f "$(DX9MT_STEAM_LOG)" ]; then \
		stat -f "%Sm" -t "%Y-%m-%d %H:%M:%S" "$(DX9MT_STEAM_LOG)"; \
		rg -n -i "err:|Unhandled exception|c000|steam exited|dx9mt/" "$(DX9MT_STEAM_LOG)" || tail -n 80 "$(DX9MT_STEAM_LOG)"; \
	else \
		echo "(no steam log yet)"; \
	fi

analyze-logs:
	@if command -v uv >/dev/null 2>&1; then \
		uv run tools/analyze_dx9mt_log.py "$(DX9MT_RUNTIME_LOG)" --top "$(DX9MT_ANALYZE_TOP)" 2>/dev/null || \
			python3 tools/analyze_dx9mt_log.py "$(DX9MT_RUNTIME_LOG)" --top "$(DX9MT_ANALYZE_TOP)"; \
	else \
		python3 tools/analyze_dx9mt_log.py "$(DX9MT_RUNTIME_LOG)" --top "$(DX9MT_ANALYZE_TOP)"; \
	fi

show-fnv-dx9mt-override: wine-restart
	@echo "Global Wine DllOverrides:"; \
	"$(WINE)" reg query "HKCU\Software\Wine\DllOverrides" || true; \
	echo "steam.exe override:"; \
	"$(WINE)" reg query "HKCU\Software\Wine\AppDefaults\steam.exe\DllOverrides" || true; \
	echo "FalloutNV.exe override:"; \
	"$(WINE)" reg query "HKCU\Software\Wine\AppDefaults\FalloutNV.exe\DllOverrides" || true; \
	echo "FalloutNVLauncher.exe override:"; \
	"$(WINE)" reg query "HKCU\Software\Wine\AppDefaults\FalloutNVLauncher.exe\DllOverrides" || true

clear:
	@-pkill -f dx9mt_metal_viewer 2>/dev/null || true
	@-rm -f "$(DX9MT_METAL_IPC_FILE)"
	@-"$(WINESERVER)" --kill
