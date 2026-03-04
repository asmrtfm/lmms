# LMMS Rust Rewrite Plan

## Decision: Rewrite in Rust

After evaluating the LMMS codebase (~912 C/C++ source files, 58 plugins, real-time audio engine, Qt GUI), **Rust** was chosen as the target language.

## Why Rust

| Concern | C++ (current) | Rust |
|---|---|---|
| Memory safety | Manual, error-prone | Compile-time guarantees, no GC |
| Real-time audio | Good (no GC) | Equally good (no GC, zero-cost abstractions) |
| Concurrency | Data races possible | Fearless concurrency via ownership model |
| C/C++ interop | N/A | Excellent FFI — enables incremental migration |
| Package ecosystem | CMake + vcpkg | Cargo (unified build, test, dependency management) |
| Cross-platform | CMake configs | First-class via Cargo targets |

### Key advantages for this project

- **No garbage collector** — critical for glitch-free real-time audio processing
- **Ownership model** prevents use-after-free and data races in the audio graph
- **Incremental migration** — Rust can call C/C++ and vice versa, so the rewrite can happen module-by-module
- **Cargo** replaces the 851-line CMakeLists.txt with a simpler, more maintainable build system
- **Strong audio ecosystem** — crates like `cpal`, `rodio`, `dasp`, and `nih-plug` cover audio I/O, DSP, and plugin formats

## Rust Crate Mapping

| Current Dependency | Rust Equivalent |
|---|---|
| Qt (GUI) | `iced`, `egui`, or `slint` |
| PortAudio / ALSA / JACK | `cpal` (cross-platform audio I/O) |
| fftw3 | `rustfft` |
| libsndfile | `hound` (WAV), `symphonia` (multi-format) |
| libsamplerate | `rubato` |
| fluidsynth | `fluidlite-rs` or FFI binding |
| libvorbis / libogg | `lewton` (Vorbis decoder), `ogg` |
| mp3lame | `mp3lame-encoder` or FFI |
| LV2 / LADSPA / VST | `nih-plug` (plugin framework), `lv2` crate |
| SDL2 | `sdl2` crate (if needed) |
| zlib | `flate2` |

## Proposed Module Structure

```
lmms-rs/
├── Cargo.toml                  (workspace root)
├── crates/
│   ├── lmms-core/              (audio engine, project model, transport)
│   ├── lmms-dsp/               (DSP primitives, filters, oscillators)
│   ├── lmms-midi/              (MIDI I/O and processing)
│   ├── lmms-audio-io/          (audio backend abstraction via cpal)
│   ├── lmms-plugin-host/       (LV2/VST/LADSPA plugin hosting)
│   ├── lmms-gui/               (UI layer)
│   ├── lmms-project/           (project file read/write, .mmp compat)
│   └── lmms-app/               (application entry point)
├── plugins/
│   ├── triple-oscillator/
│   ├── bitcrush/
│   ├── compressor/
│   ├── delay/
│   ├── eq/
│   └── ...                     (58 plugins as separate crates)
└── tests/
```

## Migration Strategy

### Phase 1 — Foundation
- Set up Cargo workspace
- Implement `lmms-core` (sample buffers, audio graph, transport)
- Implement `lmms-dsp` (oscillators, filters, envelopes)
- Implement `lmms-audio-io` with `cpal`

### Phase 2 — Plugin System
- Define Rust plugin trait
- Port built-in plugins (start with TripleOscillator, Bitcrush)
- Add LV2/VST hosting via FFI

### Phase 3 — Project Format
- Implement .mmp file parser for backward compatibility
- Define new native project format (likely MessagePack or CBOR)

### Phase 4 — GUI
- Build UI with chosen framework (iced or egui)
- Implement piano roll, pattern editor, mixer, song editor
- MIDI controller mapping

### Phase 5 — Parity & Polish
- Port remaining plugins
- Full cross-platform testing (Linux, Windows, macOS)
- Performance benchmarking against C++ version

## Alternatives Considered

- **Go** — GC pauses disqualify it for real-time audio
- **Zig** — Immature ecosystem, limited GUI/audio libraries
- **C# (.NET)** — GC latency concerns, less control over memory layout
- **Staying in C++** — Viable, but misses opportunity for memory safety and modernized tooling
