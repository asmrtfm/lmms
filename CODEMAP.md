# LMMS CODEMAP

**~128K lines of C++17/Qt5** | 308 headers | 258 source files | 56 plugins

## Top-Level Layout

```
CMakeLists.txt          Root build config
include/                All public headers (flat directory, 308 .h files)
src/
  core/                 Engine, models, audio, MIDI, serialization (129 .cpp)
  gui/                  UI components across 7 subdirs (121 .cpp)
  tracks/               Track type implementations (5 .cpp)
  3rdparty/             Vendored libs (hiir, jack2, rpmalloc, qt5-x11embed)
plugins/                57 instrument/effect/tool plugins (each self-contained)
data/                   Themes, presets, samples, translations (34 languages)
tests/                  Qt Test unit tests (6 tests)
cmake/                  Build modules, find scripts, platform packaging
tools/                  Python conversion utilities (SQLite migration)
```

## Architecture Overview

**Model-View pattern** throughout. `Model` emits `dataChanged`/`propertiesChanged`; `ModelView` observes. `AutomatableModel` is the key subclass for parameters that can be automated.

**Audio pipeline**: `AudioEngine::renderNextBuffer()` → `Song::processNextBuffer()` → worker threads process `AudioPort`, `MixerChannel`, `PlayHandle` jobs. GUI/audio thread sync via `requestChangeInModel()`/`doneChangeInModel()`.

**Central singletons**: `Engine` provides `getSong()`, `audioEngine()`, `mixer()`, `patternStore()`. `GuiApplication` provides `mainWindow()`, `songEditor()`, `pianoRoll()`, etc.

---

## src/core/ — Engine & Models (129 files)

### Core Engine
| File | Purpose |
|------|---------|
| `main.cpp` | Application entry point and initialization |
| `Engine.cpp` | Central engine singleton, subsystem initialization |
| `Song.cpp` | Root model: all tracks, project metadata, playback control |

### Audio Engine & Rendering
| File | Purpose |
|------|---------|
| `AudioEngine.cpp` | Main audio loop, PlayHandle management, worker dispatch |
| `AudioEngineWorkerThread.cpp` | Worker threads for parallel audio processing |
| `AudioEngineProfiler.cpp` | Audio performance profiling |
| `RenderManager.cpp` | Export orchestration (CLI and GUI) |
| `ProjectRenderer.cpp` | Renders project to audio file |

### Audio Backends (`audio/`)
| File | Purpose |
|------|---------|
| `AudioDevice.cpp` | Base class for audio output devices |
| `AudioPort.cpp` | Audio port abstraction |
| `AudioAlsa.cpp` | ALSA PCM output |
| `AudioJack.cpp` | JACK transport |
| `AudioPulseAudio.cpp` | PulseAudio output |
| `AudioPortAudio.cpp` | PortAudio output |
| `AudioSdl.cpp` | SDL output |
| `AudioOss.cpp` | OSS output |
| `AudioSndio.cpp` | sndio output |
| `AudioSoundIo.cpp` | libsoundio output |
| `AudioFileDevice.cpp` | Base for file-based audio output |
| `AudioSampleRecorder.cpp` | Record to RAM |
| `AudioFileWave.cpp` | WAV encoder |
| `AudioFileFlac.cpp` | FLAC encoder |
| `AudioFileMP3.cpp` | MP3 encoder |
| `AudioFileOgg.cpp` | OGG/Vorbis encoder |

### Audio Processing & Mixing
| File | Purpose |
|------|---------|
| `Mixer.cpp` | Mixer channels, sends, master output |
| `MixHelpers.cpp` | Buffer mixing utilities |
| `BufferManager.cpp` | Reusable audio buffer pool |
| `AudioResampler.cpp` | libsamplerate wrapper |
| `RingBuffer.cpp` | Ring buffer for audio data |
| `ValueBuffer.cpp` | Buffer of automation values |

### Samples
| File | Purpose |
|------|---------|
| `Sample.cpp` | Audio sample model and metadata |
| `SampleBuffer.cpp` | Raw audio sample storage |
| `SampleDecoder.cpp` | Multi-format audio file decoding |
| `SampleClip.cpp` | Clip referencing an audio sample |
| `SamplePlayHandle.cpp` | Plays a sample |
| `SampleRecordHandle.cpp` | Records audio input |

### Synthesis
| File | Purpose |
|------|---------|
| `Oscillator.cpp` | Software oscillator (sine/square/tri/saw) |
| `BandLimitedWave.cpp` | Anti-aliased waveform generation |
| `DrumSynth.cpp` | DrumSynth .ds file renderer |
| `Piano.cpp` | Piano keyboard model |

### Instruments & Effects
| File | Purpose |
|------|---------|
| `Instrument.cpp` | Base class for instrument plugins |
| `InstrumentFunctions.cpp` | Arpeggiator, chord models |
| `InstrumentSoundShaping.cpp` | ADSR envelope + filter controls |
| `InstrumentPlayHandle.cpp` | Drives instrument playback |
| `EnvelopeAndLfoParameters.cpp` | Envelope/LFO parameter model |
| `Effect.cpp` | Base class for effect plugins |
| `EffectChain.cpp` | Ordered chain of effects |

### Playback & Notes
| File | Purpose |
|------|---------|
| `PlayHandle.cpp` | Base: represents a playing sound |
| `Note.cpp` | MIDI note (pitch, length, volume, panning) |
| `NotePlayHandle.cpp` | Plays a single note on an instrument |
| `PresetPreviewPlayHandle.cpp` | Preview preset sounds |

### Clips & Patterns
| File | Purpose |
|------|---------|
| `Clip.cpp` | Base clip on timeline |
| `PatternClip.cpp` | References a pattern in PatternStore |
| `PatternStore.cpp` | Container of patterns (beat/step sequencer backend) |
| `AutomationClip.cpp` | Holds automation curves |
| `AutomationNode.cpp` | Single point on automation curve |
| `SampleClip.cpp` | Audio sample on timeline |

### Automation & Controllers
| File | Purpose |
|------|---------|
| `AutomatableModel.cpp` | Parameter automation support (central to system) |
| `Controller.cpp` | External parameter control source |
| `ControllerConnection.cpp` | Links controllers to models |
| `LfoController.cpp` | LFO as controller source |
| `PeakController.cpp` | Peak follower as controller source |
| `InlineAutomation.cpp` | Inline automation curves |

### Tracks
| File | Purpose |
|------|---------|
| `Track.cpp` | Base track: clips, muting, soloing, colors, save/load |
| `TrackContainer.cpp` | Collection of tracks (Song or PatternStore) |

### Models
| File | Purpose |
|------|---------|
| `Model.cpp` | Base model with signals |
| `ModelVisitor.cpp` | Visitor pattern for model tree |
| `ComboBoxModel.cpp` | Dropdown model |
| `MeterModel.cpp` | Time signature model |
| `TempoSyncKnobModel.cpp` | Tempo-synced parameter |
| `LinkedModelGroups.cpp` | Groups of linked parameters |

### Plugin System
| File | Purpose |
|------|---------|
| `Plugin.cpp` | Plugin loading and descriptor management |
| `PluginFactory.cpp` | Plugin instantiation factory |
| `PluginIssue.cpp` | Plugin error/issue reporting |
| `ToolPlugin.cpp` | Base for tool-type plugins |
| `RemotePlugin.cpp` | IPC for out-of-process plugins (VST) |
| `VstSyncController.cpp` | VST transport sync |

### LADSPA Support
| File | Purpose |
|------|---------|
| `LadspaManager.cpp` | LADSPA plugin discovery and loading |
| `Ladspa2LMMS.cpp` | LADSPA → LMMS adapter |
| `LadspaControl.cpp` | LADSPA port control model |

### LV2 Support (`lv2/`)
| File | Purpose |
|------|---------|
| `Lv2Manager.cpp` | LV2 plugin discovery |
| `Lv2ControlBase.cpp` | LV2 parameter handling |
| `Lv2Proc.cpp` | LV2 audio processing |
| `Lv2Ports.cpp` | LV2 port mapping |
| `Lv2Features.cpp` | LV2 feature negotiation |
| `Lv2Evbuf.cpp` | LV2 event buffers |
| `Lv2Options.cpp` | LV2 options |
| `Lv2SubPluginFeatures.cpp` | LV2 sub-plugin discovery |
| `Lv2UridCache.cpp` | LV2 URID caching |
| `Lv2UridMap.cpp` | LV2 URID mapping |
| `Lv2Worker.cpp` | LV2 worker threads |
| `Lv2Basics.cpp` | LV2 core utilities |

### MIDI (`midi/`)
| File | Purpose |
|------|---------|
| `MidiPort.cpp` | MIDI port abstraction |
| `MidiClient.cpp` | Base MIDI client |
| `MidiController.cpp` | MIDI CC message handling |
| `MidiEventToByteSeq.cpp` | MIDI event serialization |
| `MidiAlsaSeq.cpp` | ALSA sequencer backend |
| `MidiAlsaRaw.cpp` | ALSA raw MIDI backend |
| `MidiJack.cpp` | JACK MIDI backend |
| `MidiOss.cpp` | OSS MIDI backend |
| `MidiWinMM.cpp` | Windows MME backend |
| `MidiApple.cpp` | CoreMIDI backend |
| `MidiSndio.cpp` | sndio MIDI backend |

### Serialization & Data
| File | Purpose |
|------|---------|
| `DataFile.cpp` | Project file (.mmp/.mmpz) load/save, format upgrades |
| `SerializingObject.cpp` | XML serialization base |
| `JournallingObject.cpp` | Undo/redo journaling |
| `ProjectJournal.cpp` | Project undo/redo history |
| `ProjectVersion.cpp` | Version comparison for upgrades |
| `SqliteToXml.cpp` | SQLite→XML conversion (custom) |
| `XmlToSqlite.cpp` | XML→SQLite conversion (custom) |

### Music Theory
| File | Purpose |
|------|---------|
| `Scale.cpp` | Musical scale definitions |
| `Keymap.cpp` | Key-to-note mapping |
| `Microtuner.cpp` | Microtonality support |

### Timing
| File | Purpose |
|------|---------|
| `Timeline.cpp` | Timeline model |
| `TimePos.cpp` | Bar/beat/tick position |
| `Metronome.cpp` | Click track |
| `MicroTimer.cpp` | High-resolution timer |
| `StepRecorder.cpp` | Step-by-step recording |

### Utilities
| File | Purpose |
|------|---------|
| `ConfigManager.cpp` | Application settings |
| `Clipboard.cpp` | Copy/paste for clips and notes |
| `PathUtil.cpp` | File path resolution |
| `FileSearch.cpp` | File system search |
| `ImportFilter.cpp` | Base import filter |
| `base64.cpp` | Base64 encode/decode |
| `fft_helpers.cpp` | FFT utilities |
| `PerfLog.cpp` | Performance logging |
| `ThreadPool.cpp` | Worker thread pool |
| `LocklessAllocator.cpp` | Lock-free memory allocator |
| `LmmsSemaphore.cpp` | Semaphore |
| `UpgradeExtendedNoteRange.cpp` | Legacy project upgrade |

---

## src/gui/ — User Interface (121 files)

### Main Windows
| File | Purpose |
|------|---------|
| `MainWindow.cpp` | Application main window, toolbars, menus |
| `MainApplication.cpp` | QApplication event handling |
| `GuiApplication.cpp` | GUI initialization, editor creation |
| `SubWindow.cpp` | MDI sub-window with themed title bar |

### Editors (`editors/`)
| File | Purpose |
|------|---------|
| `Editor.cpp` | Base editor class |
| `SongEditor.cpp` | Song-level arrangement timeline |
| `PianoRoll.cpp` | MIDI note editor |
| `PatternEditor.cpp` | Beat/step pattern grid |
| `AutomationEditor.cpp` | Automation curve editor |
| `TrackContainerView.cpp` | Container displaying all track views |
| `TimeLineWidget.cpp` | Timeline ruler and markers |
| `PositionLine.cpp` | Playback position indicator |
| `Rubberband.cpp` | Selection rectangle |
| `StepRecorderWidget.cpp` | Step recording UI |

### Clip Views (`clips/`)
| File | Purpose |
|------|---------|
| `ClipView.cpp` | Base clip visualization |
| `MidiClipView.cpp` | MIDI clip (notes preview, beat mode) |
| `PatternClipView.cpp` | Pattern clip in song editor |
| `SampleClipView.cpp` | Sample waveform clip |
| `AutomationClipView.cpp` | Automation curve clip |

### Track Views (`tracks/`)
| File | Purpose |
|------|---------|
| `TrackView.cpp` | Base track view |
| `InstrumentTrackView.cpp` | Instrument track UI |
| `SampleTrackView.cpp` | Sample track UI |
| `AutomationTrackView.cpp` | Automation track UI |
| `PatternTrackView.cpp` | Pattern track UI |
| `TrackContentWidget.cpp` | Main content area with clips |
| `TrackOperationsWidget.cpp` | Mute/solo/color/menu buttons |
| `TrackGrip.cpp` | Drag handle for reordering |
| `TrackLabelButton.cpp` | Track name label |
| `TrackRenameLineEdit.cpp` | Inline rename field |
| `FadeButton.cpp` | Animated fade button |

### Instrument Views (`instrument/`)
| File | Purpose |
|------|---------|
| `InstrumentTrackWindow.cpp` | Instrument settings window |
| `InstrumentView.cpp` | Base instrument plugin view |
| `InstrumentSoundShapingView.cpp` | ADSR/filter controls |
| `InstrumentFunctionViews.cpp` | Arpeggiator/chord UI |
| `InstrumentMidiIOView.cpp` | MIDI I/O config |
| `InstrumentTuningView.cpp` | Tuning/transpose settings |
| `EnvelopeAndLfoView.cpp` | Envelope + LFO editor |
| `EnvelopeGraph.cpp` | ADSR curve display |
| `LfoGraph.cpp` | LFO waveform display |
| `PianoView.cpp` | Interactive piano keyboard |

### Dialogs (`modals/`)
| File | Purpose |
|------|---------|
| `SetupDialog.cpp` | Application settings |
| `ExportProjectDialog.cpp` | Audio export dialog |
| `EffectSelectDialog.cpp` | Effect browser/picker |
| `ControllerConnectionDialog.cpp` | Controller↔parameter linking |
| `AboutDialog.cpp` | About screen |
| `FileDialog.cpp` | File open/save |
| `VersionedSaveDialog.cpp` | Versioned save |
| `RenameDialog.cpp` | Rename dialog |
| `ColorChooser.cpp` | Color picker |

### Menus (`menus/`)
| File | Purpose |
|------|---------|
| `RecentProjectsMenu.cpp` | Recent projects list |
| `TemplatesMenu.cpp` | New-from-template |
| `MidiPortMenu.cpp` | MIDI port selection |

### Widgets (`widgets/`) — 31 reusable controls
| File | Purpose |
|------|---------|
| `Knob.cpp` | Rotary knob (most-used control) |
| `Fader.cpp` | Volume fader slider |
| `ComboBox.cpp` | Dropdown selector |
| `LcdWidget.cpp` | LCD-style number display |
| `LcdSpinBox.cpp` | LCD integer spinner |
| `LcdFloatSpinBox.cpp` | LCD float spinner |
| `AutomatableButton.cpp` | Button with automation |
| `AutomatableSlider.cpp` | Slider with automation |
| `PixmapButton.cpp` | Image-based button |
| `ToolButton.cpp` | Toolbar button |
| `NStateButton.cpp` | Multi-state toggle |
| `GroupBox.cpp` | Group container |
| `TabBar.cpp` / `TabWidget.cpp` | Tab containers |
| `Graph.cpp` | 2D waveform/curve display |
| `Oscilloscope.cpp` | Real-time waveform |
| `PeakIndicator.cpp` | Peak level meter |
| `CPULoadWidget.cpp` | CPU usage display |
| `CustomTextKnob.cpp` | Knob with text |
| `TempoSyncKnob.cpp` | BPM-synced knob |
| `MeterDialog.cpp` | Time signature |
| `SimpleTextFloat.cpp` / `TextFloat.cpp` | Floating labels |
| `TimeDisplayWidget.cpp` | Playback time |
| `LeftRightNav.cpp` | Arrow navigation |
| `CaptionMenu.cpp` | Menu with header |
| `LedCheckBox.cpp` | LED-style checkbox |
| `BarModelEditor.cpp` | Float value editor |
| `FloatModelEditorBase.cpp` | Base for float editors |
| `TempoSyncBarModelEditor.cpp` | Tempo-synced editor |
| `MixerChannelLcdSpinBox.cpp` | Mixer channel selector |

### Mixer & Effects UI
| File | Purpose |
|------|---------|
| `MixerView.cpp` | Mixer window |
| `MixerChannelView.cpp` | Single mixer channel strip |
| `EffectRackView.cpp` | Effects chain display |
| `EffectView.cpp` | Single effect controls |
| `EffectControlDialog.cpp` | Effect settings dialog |
| `SendButtonIndicator.cpp` | Mixer send indicator |

### Other GUI
| File | Purpose |
|------|---------|
| `FileBrowser.cpp` | File/preset/sample browser panel |
| `PluginBrowser.cpp` | Plugin browser panel |
| `SideBar.cpp` / `SideBarWidget.cpp` | Side panel |
| `ProjectNotes.cpp` | Project notes editor |
| `ControllerRackView.cpp` | Controller management |
| `ControllerView.cpp` | Individual controller |
| `ControllerDialog.cpp` | Controller config |
| `LfoControllerDialog.cpp` | LFO controller config |
| `PeakControllerDialog.cpp` | Peak controller config |
| `MidiCCRackView.cpp` | MIDI CC controls |
| `MidiSetupWidget.cpp` | MIDI device config |
| `MicrotunerConfig.cpp` | Tuning config |
| `AutomatableModelView.cpp` | Automatable widget base |
| `ModelView.cpp` | View base class |
| `LinkedModelGroupViews.cpp` | Linked model views |
| `ControlLayout.cpp` | Control layout manager |
| `Controls.cpp` | Labeled control wrappers |
| `StringPairDrag.cpp` | Drag-and-drop support |
| `SampleLoader.cpp` | Audio file loading |
| `SampleWaveform.cpp` | Waveform drawing |
| `SampleTrackWindow.cpp` | Sample track settings |
| `LadspaControlView.cpp` | LADSPA parameter control |
| `Lv2ViewBase.cpp` | LV2 plugin view base |
| `RowTableView.cpp` | Table view |
| `LmmsPalette.cpp` | Theme palette |
| `LmmsStyle.cpp` | Application style |
| `embed.cpp` | Resource embedding |
| `ActionGroup.cpp` | QAction grouping |
| `AudioAlsaSetupWidget.cpp` | ALSA config widget |
| `AudioDeviceSetupWidget.cpp` | Audio device config base |

---

## src/tracks/ — Track Implementations (5 files)

| File | Purpose |
|------|---------|
| `InstrumentTrack.cpp` | Instrument + MIDI clips + effects + sound shaping |
| `MidiClip.cpp` | MIDI note container with serialization |
| `SampleTrack.cpp` | Audio sample arrangement + effects |
| `PatternTrack.cpp` | Pattern reference for song timeline |
| `AutomationTrack.cpp` | Parameter automation track |

---

## plugins/ — 57 Plugins

### Instruments (24)
| Plugin | Description |
|--------|-------------|
| `TripleOscillator` | Triple oscillator with modulation (default instrument) |
| `AudioFileProcessor` | Sample playback instrument |
| `ZynAddSubFx` | Complex additive/subtractive synthesis |
| `Sf2Player` | SoundFont 2 player |
| `GigPlayer` | GigaSampler format player |
| `Monstro` | Wavetable/noise hybrid synth |
| `Watsyn` | Wavetable synthesis |
| `OpulenZ` | FM synthesis (OPL2) |
| `Organic` | Additive synthesis |
| `Lb302` | TB-303 acid bass |
| `Kicker` | Drum synthesizer |
| `BitInvader` | Bit-depth synthesis |
| `FreeBoy` | Game Boy emulation |
| `Nes` | NES emulation |
| `Sid` | C64 SID emulation |
| `Sfxr` | Chiptune sound effects |
| `SlicerT` | Sample slicer |
| `Vibed` | Vibraphone |
| `Patman` | Pattern/drum machine |
| `Xpressive` | Expression-based synthesis |
| `Stk/Mallets` | Physical modeling |
| `Vestige` | VST instrument host |
| `CarlaRack` | Carla plugin host (rack) |
| `CarlaPatchbay` | Carla plugin host (patchbay) |

### Effects (23)
| Plugin | Description |
|--------|-------------|
| `Amplifier` | Gain control |
| `BassBooster` | Bass enhancement |
| `Bitcrush` | Bit reduction |
| `Compressor` | Dynamic range compression |
| `CrossoverEQ` | Multi-band crossover EQ |
| `Delay` | Time-based delay |
| `Dispersion` | Frequency dispersion |
| `DualFilter` | Dual filter + LFO |
| `DynamicsProcessor` | Advanced dynamics |
| `Eq` | Parametric EQ |
| `Flanger` | Flange effect |
| `GranularPitchShifter` | Granular pitch shift |
| `LOMM` | Multiband mastering |
| `MultitapEcho` | Multi-tap echo |
| `ReverbSC` | Schroeder reverb |
| `StereoEnhancer` | Stereo width |
| `StereoMatrix` | Stereo matrix mixer |
| `WaveShaper` | Waveshaping distortion |
| `PeakControllerEffect` | Peak detector for automation |
| `SpectrumAnalyzer` | Frequency spectrum display |
| `LadspaEffect` | LADSPA plugin host |
| `Lv2Effect` | LV2 plugin host |
| `VstEffect` | VST effect host |

### Import/Export (3)
| Plugin | Description |
|--------|-------------|
| `MidiImport` | MIDI file importer |
| `MidiExport` | MIDI file exporter |
| `HydrogenImport` | Hydrogen drum machine importer |

### Tools (2)
| Plugin | Description |
|--------|-------------|
| `LadspaBrowser` | LADSPA plugin browser |
| `TapTempo` | BPM tap tempo tool |

### Base Libraries (3)
| Plugin | Description |
|--------|-------------|
| `CarlaBase` | Shared Carla hosting infrastructure |
| `VstBase` | Shared VST hosting infrastructure |
| `Lv2Instrument` | LV2 instrument wrapper |

---

## tests/ — Unit Tests (6 tests)

| File | Tests |
|------|-------|
| `tests/src/core/ArrayVectorTest.cpp` | ArrayVector container |
| `tests/src/core/AutomatableModelTest.cpp` | Automation model system |
| `tests/src/core/MathTest.cpp` | Math utilities |
| `tests/src/core/ProjectVersionTest.cpp` | Version comparison |
| `tests/src/core/RelativePathsTest.cpp` | Path resolution |
| `tests/src/tracks/AutomationTrackTest.cpp` | Automation track |

Scripted checks in `tests/scripted/`: namespace consistency, translatable strings.

---

## cmake/ — Build System

### Key Modules (`cmake/modules/`)
| File | Purpose |
|------|---------|
| `BuildPlugin.cmake` | `BUILD_PLUGIN()` macro for plugin builds |
| `PluginList.cmake` | Plugin enumeration and selection |
| `DetectMachine.cmake` | Platform/architecture detection |
| `ErrorFlags.cmake` | Compiler warning/error flags |
| `CompileCache.cmake` | ccache/sccache integration |
| `CheckSubmodules.cmake` | Git submodule verification |
| `GenQrc.cmake` | Qt resource generation |
| 14 `Find*.cmake` files | Dependency detection (ALSA, FFTW, FluidSynth, etc.) |

### Platform Packaging
| Directory | Purpose |
|-----------|---------|
| `cmake/apple/` | macOS .dmg packaging, icons, plist |
| `cmake/linux/` | Desktop integration, .spec, icons |
| `cmake/nsis/` | Windows NSIS installer |
| `cmake/toolchains/` | MinGW cross-compilation |

---

## tools/ — Conversion Utilities

| File | Purpose |
|------|---------|
| `lmms_convert.py` | XML→SQLite project converter |
| `lmms_export.py` | SQLite→XML export bridge |
| `lmms_schema.sql` | SQLite schema for project database |
| `test_conversion.py` | Round-trip conversion tests |
| `PHASE2_DESIGN.md` | SQLite migration architecture doc |

---

## Key Relationships

```
Song ──────── TrackContainer ──── Track[] ──── Clip[]
  │                                  │
  │         ┌── InstrumentTrack ─── MidiClip (notes)
  │         ├── SampleTrack ─────── SampleClip (audio)
  │         ├── AutomationTrack ─── AutomationClip (curves)
  │         └── PatternTrack ────── PatternClip (pattern ref)
  │
  ├── PatternStore ── TrackContainer ── InstrumentTrack[] ── MidiClip[]
  │                                     (beat/step patterns)
  │
  ├── Mixer ── MixerChannel[] ── EffectChain ── Effect[]
  │
  └── Timeline, Controllers[], ProjectJournal

AudioEngine ── PlayHandle[] ── NotePlayHandle / SamplePlayHandle / InstrumentPlayHandle
    │
    └── AudioEngineWorkerThread[] ── process AudioPort, MixerChannel, PlayHandle

Plugin ── Instrument | Effect | ImportFilter | ExportFilter | ToolPlugin
    │
    └── loaded by PluginFactory, described by Plugin::Descriptor
```

## Data Flow: Note Playback
1. `Song::processNextBuffer()` iterates tracks
2. `InstrumentTrack` finds active `MidiClip` at current position
3. Notes in range create `NotePlayHandle` instances
4. `NotePlayHandle` calls `Instrument::playNote()` on the plugin
5. Plugin generates audio into `AudioPort` buffer
6. `EffectChain` processes the buffer
7. Buffer routes to `MixerChannel` → master → `AudioDevice`

## Data Flow: Project Save/Load
1. `Song::saveProject()` → `DataFile` creates XML DOM
2. Each `Track` calls `saveTrack()` → serializes settings + clips
3. `DataFile::writeFile()` compresses (zlib for .mmpz) and writes
4. Load: `DataFile` reads + decompresses → `upgradeElement()` chain → `Song::loadProject()`
5. `DataFile.cpp` contains all format upgrade logic (version-gated transforms)
