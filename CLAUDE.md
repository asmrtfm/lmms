# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

LMMS (Linux MultiMedia Studio) is a free, cross-platform digital audio workstation (DAW) written in C++17 using Qt5. It supports Windows, macOS, and Linux.

## Build Commands

```bash
# Configure (out-of-tree build, recommended)
mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=../target

# Build
cmake --build build

# Run tests
cd build/tests && ctest --output-on-failure -j2

# Run a single test
cd build/tests && ctest -R <TestName> --output-on-failure
# Test names: ArrayVectorTest, AutomatableModelTest, MathTest,
#             ProjectVersionTest, RelativePathsTest, AutomationTrackTest

# Install locally (non-root)
cmake --build build --target install

# CI uses these flags:
# -DUSE_WERROR=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

Submodules are required: clone with `--recurse-submodules` or run `git submodule update --init --recursive`.

## Code Style

Enforced by `.clang-format`. Key rules:
- **Indentation:** Tabs, width 4
- **Line length:** 120 characters max
- **Braces:** Own line (Allman style) for classes, functions, control statements; `AfterNamespace: false`
- **Naming:** Types `UpperCamelCase`, variables `camelCase`, members `m_member`, static members `s_member`
- **Pointers:** Left-aligned (`int* ptr`, not `int *ptr`)
- **Null:** `nullptr` (not `NULL` or `0`)
- **Booleans:** `true`/`false` (not `TRUE`/`1`/`0`)
- **Includes order:** Own header first, then `<system>`, then `"project"` (separated by blank lines)
- **Flow control:** Space after keyword: `if (x)` not `if(x)`
- **Return:** No parentheses: `return x;` not `return (x);`

Special comments: `TODO`, `FIXME`, `HACK`, `Workaround`, `// TODO C++##:`, `// TODO CMake X.XX:`

## Architecture

### Core Pattern: Model-View
- **Model** (`Model` base): Emits `dataChanged`/`propertiesChanged` signals
- **ModelView**: Holds reference to model, overrides `modelChanged()` for updates
- **AutomatableModel**: Parameters that support automation, the most important model subclass

### Audio Engine (`AudioEngine`)
- Main loop: `renderNextBuffer()` — removes finished PlayHandles, calls `Song::processNextBuffer()`, queues worker jobs
- **AudioEngineWorkerThread**: Processes `AudioPort`, `MixerChannel`, and `PlayHandle` jobs
- GUI/audio thread sync via `requestChangeInModel()`/`doneChangeInModel()`

### Track System
- Track types: `InstrumentTrack`, `SampleTrack`, `AutomationTrack`, `BBTrack`
- Song tracks in `Song` (via `Engine::getSong()`), BB tracks in `BBTrackContainer` (via `Engine::getBBTrackContainer()`)
- Clips: `PatternClip`, `SampleClip`, `AutomationClip`

### Plugin System
Each plugin is a subdirectory under `plugins/` with:
- `CMakeLists.txt` using `BUILD_PLUGIN()` macro
- Plugin descriptor (`Plugin::Descriptor`) exported via `extern "C"`
- Factory function `lmms_plugin_main()` exported via `extern "C"`
- Types: `Plugin::Instrument`, `Plugin::Effect`

### Directory Layout
- `include/` — All public headers (centralized, ~354 files)
- `src/core/` — Engine, models, audio processing
- `src/gui/` — UI components (clips/, tracks/, editors/, widgets/)
- `src/tracks/` — Track implementations
- `plugins/` — 57+ instrument and effect plugins
- `data/` — Resources (themes, presets, samples, translations)
- `tests/src/` — Qt Test-based unit tests
- `cmake/modules/` — Build helpers (`BuildPlugin.cmake`, `PluginList.cmake`, `DetectMachine.cmake`)

### Key Subsystems
- **Automation**: `AutomatableModel` → `AutomationClip` → `AutomationNode`, with `Controller`/`ControllerConnection`
- **Effects chain**: `EffectChain` manages `Effect` instances per track/channel
- **MIDI**: Handled through various platform backends
- **Plugin formats**: VST (via RemotePlugin), LV2, LADSPA, Carla host

## Testing

Tests use Qt Test framework. Test files live in `tests/src/`. Each test is a class with `private slots` for test methods. Use `QCOMPARE()`, `QVERIFY()`, and standard Qt Test macros.

## Dependencies

Required: Qt5 (>=5.9), libsndfile (>=1.0.18), fftw3, libsamplerate (>=0.1.7), CMake (>=3.13)

Optional audio backends: ALSA, JACK, PulseAudio, PortAudio, SDL2, CoreAudio, WinMM

Optional plugin support: VST, LV2 (lilv/suil), LADSPA, Carla, FluidSynth (SF2), libgig
