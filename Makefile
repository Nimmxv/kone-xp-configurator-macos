CC := xcrun clang
CFLAGS := -std=c17 -Wall -Wextra -Wpedantic -O2
ARCH_FLAGS := -arch arm64 -arch x86_64
FRAMEWORKS := -framework Foundation -framework IOKit -framework CoreFoundation
BUILD_DIR := build
TARGET := $(BUILD_DIR)/konexp
PACKAGE := outputs/Kone XP Configurator.zip

.PHONY: all app clean

all: $(TARGET)

app: $(TARGET) AppResources/Info.plist AppResources/PkgInfo AppResources/AppIcon.icns AppResources/launcher.m
	@set -e; \
	tmp=$$(mktemp -d /private/tmp/kone-configurator.XXXXXX); \
	app="$$tmp/Kone XP Configurator.app"; \
	mkdir -p "$$app/Contents/MacOS" "$$app/Contents/Resources"; \
	$(CC) $(ARCH_FLAGS) -fobjc-arc -O2 AppResources/launcher.m -o "$$app/Contents/MacOS/KoneXPConfigurator" -framework Cocoa -framework IOKit; \
	cp "$(TARGET)" "$$app/Contents/Resources/konexp-cli"; \
	cp AppResources/AppIcon.icns "$$app/Contents/Resources/AppIcon.icns"; \
	cp AppResources/Info.plist "$$app/Contents/Info.plist"; \
	cp AppResources/PkgInfo "$$app/Contents/PkgInfo"; \
	xattr -cr "$$app"; \
	codesign --force --deep --sign - "$$app"; \
	codesign --verify --deep --strict "$$app"; \
	ditto -c -k --norsrc --noextattr --noqtn --noacl --keepParent "$$app" "$(PACKAGE)"; \
	echo "Created $(PACKAGE)"

$(TARGET): Sources/KoneXPTool/main.c
	mkdir -p $(BUILD_DIR)
	$(CC) $(ARCH_FLAGS) $(CFLAGS) $< -o $@ $(FRAMEWORKS)

clean:
	rm -f $(TARGET)
