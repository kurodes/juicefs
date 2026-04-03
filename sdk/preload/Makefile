export GO111MODULE=on

REVISION := $(shell git rev-parse --short HEAD 2>/dev/null)
REVISIONDATE := $(shell git log -1 --pretty=format:'%cd' --date short 2>/dev/null)
PKG := github.com/juicedata/juicefs/pkg/version
LDFLAGS = -s -w
ifneq ($(strip $(REVISION)),)
	LDFLAGS += -X $(PKG).revision=$(REVISION) \
		   -X $(PKG).revisionDate=$(REVISIONDATE)
endif

# libsyscall_intercept configuration
# Set SYSCALL_INTERCEPT_DIR to the root directory containing lib/ and include/.
# Example: make SYSCALL_INTERCEPT_DIR=/usr/local
SYSCALL_INTERCEPT_DIR ?=
SYSCALL_INTERCEPT_INCLUDE ?= $(SYSCALL_INTERCEPT_DIR)/include
SYSCALL_INTERCEPT_LIB ?= $(SYSCALL_INTERCEPT_DIR)/lib

CGO_CFLAGS := -D_GNU_SOURCE
CGO_LDFLAGS := -lsyscall_intercept -ldl

ifneq ($(SYSCALL_INTERCEPT_DIR),)
	CGO_CFLAGS += -I$(SYSCALL_INTERCEPT_INCLUDE)
	CGO_LDFLAGS += -L$(SYSCALL_INTERCEPT_LIB) -Wl,-rpath,$(SYSCALL_INTERCEPT_LIB)
endif

# Build tags: exclude heavy optional backends to minimize binary size.
# Override with BUILD_TAGS= to include all backends.
BUILD_TAGS ?= nogateway,nowebdav,nohdfs,nosftp,nonfs,nodragonfly,nocifs,nogspt

OUTPUT := libjfs_preload.so

.PHONY: all clean

all: $(OUTPUT)

$(OUTPUT): *.go *.c ../../pkg/*/*.go
	CGO_CFLAGS="$(CGO_CFLAGS)" \
	CGO_LDFLAGS="$(CGO_LDFLAGS)" \
	go build -tags "$(BUILD_TAGS)" -buildmode=c-shared -ldflags="$(LDFLAGS)" -o $(OUTPUT) .

# Build with all backends enabled (requires all cloud SDK dependencies)
full: BUILD_TAGS =
full: $(OUTPUT)

clean:
	rm -f $(OUTPUT) libjfs_preload.h
