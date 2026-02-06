FRONTEND_CC ?= i686-w64-mingw32-gcc
BACKEND_CC ?= clang

BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj

FRONTEND_CFLAGS := -std=c11 -Wall -Wextra -Wno-unused-parameter -Iinclude -DWIN32_LEAN_AND_MEAN
FRONTEND_LDFLAGS := -shared -Wl,--kill-at -luuid
FRONTEND_DEF := src/frontend/d3d9.def

BACKEND_CFLAGS := -std=c11 -Wall -Wextra -Wno-unused-parameter -Iinclude
BACKEND_LDFLAGS := -dynamiclib

FRONTEND_SRCS := \
	src/common/log.c \
	src/backend/backend_bridge_stub.c \
	src/frontend/runtime.c \
	src/frontend/dllmain.c \
	src/frontend/d3d9.c \
	src/frontend/d3d9_device.c \
	src/frontend/d3d9_perf.c

BACKEND_SRCS := \
	src/common/log.c \
	src/backend/backend_bridge_stub.c

FRONTEND_OBJS := $(patsubst %.c,$(OBJ_DIR)/frontend/%.o,$(FRONTEND_SRCS))
BACKEND_OBJS := $(patsubst %.c,$(OBJ_DIR)/backend/%.o,$(BACKEND_SRCS))

.PHONY: all clean

all: $(BUILD_DIR)/d3d9.dll $(BUILD_DIR)/libdx9mt_unixlib.dylib

$(BUILD_DIR)/d3d9.dll: $(FRONTEND_OBJS) $(FRONTEND_DEF)
	@mkdir -p $(BUILD_DIR)
	$(FRONTEND_CC) $(FRONTEND_LDFLAGS) -o $@ $(FRONTEND_OBJS) $(FRONTEND_DEF)

$(BUILD_DIR)/libdx9mt_unixlib.dylib: $(BACKEND_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(BACKEND_CC) $(BACKEND_LDFLAGS) -o $@ $^

$(OBJ_DIR)/frontend/%.o: %.c
	@mkdir -p $(dir $@)
	$(FRONTEND_CC) $(FRONTEND_CFLAGS) -c -o $@ $<

$(OBJ_DIR)/backend/%.o: %.c
	@mkdir -p $(dir $@)
	$(BACKEND_CC) $(BACKEND_CFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILD_DIR)
