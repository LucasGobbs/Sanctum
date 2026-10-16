# Sanctum — contemplative shader scenes (Slang + goose + SDL3 GPU, C++20).
# No dependency manager: slangc from ~/.local/opt/slang, SDL3 from Homebrew,
# goose-build from the sibling checkout ../goose (GOOSE_DIR overrides).
GOOSE_DIR ?= ../goose
SLANG     ?= $(HOME)/.local/opt/slang/bin/slangc
CXX       ?= clang++
CXXFLAGS  := -std=c++20 -Wall -Wextra -Wpedantic -I$(GOOSE_DIR)/bindings/cpp/include -I/opt/homebrew/include
LDFLAGS   := -L/opt/homebrew/lib -Wl,-rpath,/opt/homebrew/lib -lSDL3
BUILD     := build

SCENES := $(filter-out shaders/common.slang,$(wildcard shaders/scenes/*.slang))

.PHONY: all run shaders clean
all: $(BUILD)/sanctum $(BUILD)/generated/stage-models

$(BUILD)/sanctum: src/main.cpp $(wildcard shaders/**/*.slang) $(GOOSE_DIR)/bindings/cpp/include/goose/*.hpp $(GOOSE_DIR)/bindings/cpp/include/goose/adapters/*.hpp
	$(CXX) $(CXXFLAGS) -o $@ src/main.cpp $(LDFLAGS)

shaders: $(BUILD)/goose.json
	$(GOOSE_DIR)/goose-build --manifest $(BUILD)/goose.json --slang $(SLANG)

$(BUILD)/goose.json: scripts/gen-manifest.sh $(SCENES)
	sh scripts/gen-manifest.sh > $@

$(BUILD)/generated/stage-models: shaders
	@mkdir -p $(BUILD)/generated
	@for r in $(BUILD)/generated/*.refl.json; do \
		[ -e "$$r" ] || continue; \
		n=$$(basename $$r .refl.json); \
		case "$$n" in *.vert|*.frag) ;; *) continue ;; esac; \
		$(GOOSE_DIR)/goose-build --analyze $$r > $(BUILD)/generated/$$n.model.json; \
	done
	touch $@

run: all shaders
	$(BUILD)/sanctum

clean:
	rm -rf $(BUILD)
