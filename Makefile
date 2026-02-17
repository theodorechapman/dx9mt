WINE := /opt/homebrew/bin/wine
WINESERVER := /opt/homebrew/bin/wineserver
WINEPREFIX := $(CURDIR)/wineprefix
WINE_SILENT := WINEDEBUG=-all "$(WINE)"

DX9MT_DIR := $(CURDIR)/dx9mt
DX9MT_DLL := $(DX9MT_DIR)/build/d3d9.dll
FNV_DIR := $(WINEPREFIX)/drive_c/Games/GOG Fallout New Vegas/Fallout New Vegas

DX9MT_RUNTIME_LOG ?= /tmp/dx9mt_runtime.log
DX9MT_LAUNCHER_LOG ?= /tmp/fnv_dx9mt_probe.log
DX9MT_ANALYZE_TOP ?= 20
DX9MT_CAPTURE_DIR ?= /tmp/dx9mt_capture
DX9MT_CAPTURE_MAX_FRAMES ?= 0
DX9MT_CAPTURE_IDLE_MS ?= 5000
DX9MT_CAPTURE_TEXT ?= 1
DX9MT_CAPTURE_RAW ?= 1

WINEDLLOVERRIDES := dxsetup.exe=d

export WINEPREFIX
export WINEDLLOVERRIDES

.DEFAULT_GOAL := run
.PHONY: run run-capture run-wine show-logs analyze-logs test clear dx9mt-build dx9mt-test install-dx9mt-fnv configure-fnv-dx9mt-override configure-fnv-wine-d3d9 show-fnv-dx9mt-override wine-restart

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
		$(WINE_SILENT) reg delete "HKCU\Software\Wine\AppDefaults\FalloutNVLauncher.exe\DllOverrides" /v $$dll /f >/dev/null 2>&1 || true; \
		$(WINE_SILENT) reg delete "HKCU\Software\Wine\AppDefaults\FalloutNV.exe\DllOverrides" /v $$dll /f >/dev/null 2>&1 || true; \
	done; \
	$(WINE_SILENT) reg add "HKCU\Software\Wine\AppDefaults\FalloutNV.exe\DllOverrides" /v d3d9 /d native,builtin /f >/dev/null; \
	$(WINE_SILENT) reg add "HKCU\Software\Wine\AppDefaults\FalloutNVLauncher.exe\DllOverrides" /v d3d9 /d native,builtin /f >/dev/null

configure-fnv-wine-d3d9: wine-restart
	@set -e; \
	for dll in d3d8 d3d9 d3d10 d3d10_1 d3d10core d3d11 d3d12 dxgi; do \
		$(WINE_SILENT) reg delete "HKCU\Software\Wine\DllOverrides" /v $$dll /f >/dev/null 2>&1 || true; \
		$(WINE_SILENT) reg delete "HKCU\Software\Wine\AppDefaults\FalloutNVLauncher.exe\DllOverrides" /v $$dll /f >/dev/null 2>&1 || true; \
		$(WINE_SILENT) reg delete "HKCU\Software\Wine\AppDefaults\FalloutNV.exe\DllOverrides" /v $$dll /f >/dev/null 2>&1 || true; \
	done; \
	$(WINE_SILENT) reg add "HKCU\Software\Wine\AppDefaults\FalloutNV.exe\DllOverrides" /v d3d9 /d builtin /f >/dev/null; \
	$(WINE_SILENT) reg add "HKCU\Software\Wine\AppDefaults\FalloutNVLauncher.exe\DllOverrides" /v d3d9 /d builtin /f >/dev/null

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
	echo "Creating Metal IPC shared file"; \
	dd if=/dev/zero of="$(DX9MT_METAL_IPC_FILE)" bs=1048576 count=16 >/dev/null 2>&1; \
	if [ -x "$(DX9MT_METAL_VIEWER)" ]; then \
		echo "Launching Metal viewer"; \
		DX9MT_CAPTURE_DIR="$(DX9MT_CAPTURE_DIR)" \
		DX9MT_CAPTURE_MAX_FRAMES="$(DX9MT_CAPTURE_MAX_FRAMES)" \
		DX9MT_CAPTURE_IDLE_MS="$(DX9MT_CAPTURE_IDLE_MS)" \
		DX9MT_CAPTURE_TEXT="$(DX9MT_CAPTURE_TEXT)" \
		DX9MT_CAPTURE_RAW="$(DX9MT_CAPTURE_RAW)" \
		"$(DX9MT_METAL_VIEWER)" &  \
		VIEWER_PID=$$!; \
		echo "Metal viewer pid $$VIEWER_PID"; \
		sleep 1; \
	else \
		echo "Metal viewer not found at $(DX9MT_METAL_VIEWER), skipping"; \
	fi; \
	echo "Launching FNV via NVSE (log: $(DX9MT_RUNTIME_LOG))"; \
	cd "$(FNV_DIR)" && \
	DX9MT_LOG_PATH="$(DX9MT_RUNTIME_LOG)" WINEDLLOVERRIDES="$(WINEDLLOVERRIDES)" \
	WINEDEBUG=+seh,+tid \
	"$(WINE)" nvse_loader.exe >/tmp/dx9mt_wine_stdout.log 2>/tmp/dx9mt_wine_stderr.log; \
	echo "Wine exited $$?"; \
	echo "=== Wine stderr (crash trace) ==="; \
	tail -60 /tmp/dx9mt_wine_stderr.log

run-capture:
	@echo "Launching with continuous capture support"; \
	echo "In the Metal viewer window: press C to start/stop capture, X to force-stop, D for one-shot frame dump"; \
	echo "Capture output root: $(DX9MT_CAPTURE_DIR)"; \
	$(MAKE) --no-print-directory run \
		DX9MT_CAPTURE_DIR="$(DX9MT_CAPTURE_DIR)" \
		DX9MT_CAPTURE_MAX_FRAMES="$(DX9MT_CAPTURE_MAX_FRAMES)" \
		DX9MT_CAPTURE_IDLE_MS="$(DX9MT_CAPTURE_IDLE_MS)" \
		DX9MT_CAPTURE_TEXT="$(DX9MT_CAPTURE_TEXT)" \
		DX9MT_CAPTURE_RAW="$(DX9MT_CAPTURE_RAW)"

run-wine: configure-fnv-wine-d3d9
	@set -e; \
	pkill -f dx9mt_metal_viewer 2>/dev/null || true; \
	echo "Launching FNV via NVSE with Wine d3d9 (builtin)"; \
	cd "$(FNV_DIR)" && \
	WINEDLLOVERRIDES="$(WINEDLLOVERRIDES)" \
	WINEDEBUG=+seh,+tid \
	"$(WINE)" nvse_loader.exe >/tmp/fnv_wine_stdout.log 2>/tmp/fnv_wine_stderr.log; \
	echo "Wine exited $$?"; \
	echo "=== Wine stderr (crash trace) ==="; \
	tail -60 /tmp/fnv_wine_stderr.log

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
	echo "FalloutNV.exe override:"; \
	"$(WINE)" reg query "HKCU\Software\Wine\AppDefaults\FalloutNV.exe\DllOverrides" || true; \
	echo "FalloutNVLauncher.exe override:"; \
	"$(WINE)" reg query "HKCU\Software\Wine\AppDefaults\FalloutNVLauncher.exe\DllOverrides" || true

clear:
	@-pkill -f dx9mt_metal_viewer 2>/dev/null || true
	@-rm -f "$(DX9MT_METAL_IPC_FILE)"
	@-"$(WINESERVER)" --kill
