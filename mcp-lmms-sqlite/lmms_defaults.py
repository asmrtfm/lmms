"""Default parameter values for LMMS instrument tracks, plugins, and effects.

These constants mirror the values LMMS writes when saving a new project.
All parameter values are strings to match the XML-attribute convention
used by the lmms_convert.py round-trip format. journallingObject entries
are omitted -- LMMS assigns them at runtime on load.

To add a new plugin's defaults:
  1. Create the instrument/effect in LMMS and save the project as .mmp
  2. Convert: python3 tools/lmms_convert.py my_project.mmp
  3. Query the DB:
       SELECT instrument_params_json FROM instrument_track
       WHERE instrument_plugin = 'pluginname';
     or for effects:
       SELECT params_json FROM effect WHERE plugin_name = 'effectname';
  4. Strip journallingObject and key entries (runtime-only)
  5. Add the cleaned dict as a new entry in the appropriate *_DEFAULTS dict
"""

import copy
import json
from typing import Any


# ============================================================
# A. Track subsystem defaults (shared by ALL instrument tracks)
# ============================================================

# Reusable envelope/LFO target template (used for elvol, elcut, elres)
_ENVELOPE_LFO_TARGET = {
    "amt": "0",
    "att": "0",
    "ctlenvamt": "0",
    "dec": "0.5",
    "hold": "0.5",
    "lamt": "0",
    "latt": "0",
    "lpdel": "0",
    "lshp": "0",
    "lspd": "0.1",
    "lspd_denominator": "4",
    "lspd_numerator": "4",
    "lspd_syncmode": "0",
    "pdel": "0",
    "rel": "0.1",
    "sustain": "0.5",
    "userwavefile": "",
    "x100": "0",
}

SOUND_SHAPING = {
    "fcut": "14000",
    "fres": "0.5",
    "ftype": "0",
    "fwet": "0",
    "elvol": dict(_ENVELOPE_LFO_TARGET),
    "elcut": dict(_ENVELOPE_LFO_TARGET),
    "elres": dict(_ENVELOPE_LFO_TARGET),
}

ARPEGGIO = {
    "arp": "0",
    "arp-enabled": "0",
    "arpcycle": "0",
    "arpdir": "0",
    "arpgate": "100",
    "arpmiss": "0",
    "arpmode": "0",
    "arprange": "1",
    "arprepeats": "1",
    "arpskip": "0",
    "arptime": "200",
    "arptime_denominator": "4",
    "arptime_numerator": "4",
    "arptime_syncmode": "0",
}

CHORD_CREATOR = {
    "chord": "0",
    "chord-enabled": "0",
    "chordrange": "1",
}

MIDI_PORT = {
    "basevelocity": "63",
    "fixedinputvelocity": "-1",
    "fixedoutputnote": "-1",
    "fixedoutputvelocity": "-1",
    "inputchannel": "0",
    "inputcontroller": "-1",
    "outputchannel": "1",
    "outputcontroller": "-1",
    "outputprogram": "1",
    "readable": "0",
    "writable": "0",
}

TRACK_EXTRA = {
    "mutedBeforeSolo": "0",
}

# MIDI CC map: cc0 through cc127 all set to "0"
_MIDI_CC_MAP = {f"cc{i}": "0" for i in range(128)}

INSTRUMENTTRACK_EXTRA = {
    "enablecc": "0",
    "enabled": "0",
    "firstkey": "0",
    "keymap": "0",
    "lastkey": "127",
    "range_import": "1",
    "scale": "0",
    "_children": {
        "midicontrollers": dict(_MIDI_CC_MAP),
    },
}


# ============================================================
# B. Instrument plugin defaults (keyed by lowercase internal name)
# ============================================================

INSTRUMENT_PLUGIN_DEFAULTS: dict[str, dict[str, Any]] = {
    "tripleoscillator": {
        "coarse0": "0",
        "coarse1": "-12",
        "coarse2": "-24",
        "finel0": "0",
        "finel1": "0",
        "finel2": "0",
        "finer0": "0",
        "finer1": "0",
        "finer2": "0",
        "modalgo1": "2",
        "modalgo2": "2",
        "modalgo3": "2",
        "pan0": "0",
        "pan1": "0",
        "pan2": "0",
        "phoffset0": "0",
        "phoffset1": "0",
        "phoffset2": "0",
        "stphdetun0": "0",
        "stphdetun1": "0",
        "stphdetun2": "0",
        "useWaveTable1": "1",
        "useWaveTable2": "1",
        "useWaveTable3": "1",
        "userwavefile0": "",
        "userwavefile1": "",
        "userwavefile2": "",
        "vol0": "33",
        "vol1": "33",
        "vol2": "33",
        "wavetype0": "0",
        "wavetype1": "0",
        "wavetype2": "0",
    },
    "kicker": {
        "click": "0.40000001",
        "decay": "440",
        "decay_denominator": "4",
        "decay_numerator": "4",
        "decay_syncmode": "0",
        "dist": "0.80000001",
        "distend": "0.80000001",
        "endfreq": "40",
        "endnote": "0",
        "env": "0.163",
        "gain": "1",
        "noise": "0",
        "slope": "0.059999999",
        "startfreq": "150",
        "startnote": "1",
        "version": "1",
    },
    "audiofileprocessor": {
        "amp": "100",
        "eframe": "1",
        "interp": "1",
        "lframe": "0",
        "looped": "0",
        "reversed": "0",
        "sframe": "0",
        "src": "",
        "stutter": "0",
    },
    "lb302": {
        "vcf_cut": "0.75",
        "vcf_res": "0.75",
        "vcf_mod": "0.1",
        "vcf_dec": "0.1",
        "shape": "8",
        "dist": "0",
        "slide_dec": "0.6",
        "slide": "0",
        "dead": "0",
        "db24": "0",
    },
    "monstro": {
        # Oscillator 1
        "o1vol": "33",
        "o1pan": "0",
        "o1crs": "0",
        "o1ftl": "0",
        "o1ftr": "0",
        "o1spo": "0",
        "o1pw": "50",
        "o1ssr": "0",
        "o1ssf": "0",
        # Oscillator 2
        "o2vol": "33",
        "o2pan": "0",
        "o2crs": "0",
        "o2ftl": "0",
        "o2ftr": "0",
        "o2spo": "0",
        "o2wav": "0",
        "o2syn": "0",
        "o2synr": "0",
        # Oscillator 3
        "o3vol": "33",
        "o3pan": "0",
        "o3crs": "0",
        "o3spo": "0",
        "o3sub": "0",
        "o3wav1": "0",
        "o3wav2": "0",
        "o3syn": "0",
        "o3synr": "0",
        # LFO 1
        "l1wav": "0",
        "l1att": "0",
        "l1rat": "1",
        "l1phs": "0",
        # LFO 2
        "l2wav": "0",
        "l2att": "0",
        "l2rat": "1",
        "l2phs": "0",
        # Envelope 1
        "e1pre": "0",
        "e1att": "0",
        "e1hol": "0",
        "e1dec": "0",
        "e1sus": "1",
        "e1rel": "0",
        "e1slo": "0",
        # Envelope 2
        "e2pre": "0",
        "e2att": "0",
        "e2hol": "0",
        "e2dec": "0",
        "e2sus": "1",
        "e2rel": "0",
        "e2slo": "0",
        # Modulation mode
        "o23mo": "0",
        # Modulation matrix: volume
        "v1e1": "0",
        "v1e2": "0",
        "v1l1": "0",
        "v1l2": "0",
        "v2e1": "0",
        "v2e2": "0",
        "v2l1": "0",
        "v2l2": "0",
        "v3e1": "0",
        "v3e2": "0",
        "v3l1": "0",
        "v3l2": "0",
        # Modulation matrix: phase
        "p1e1": "0",
        "p1e2": "0",
        "p1l1": "0",
        "p1l2": "0",
        "p2e1": "0",
        "p2e2": "0",
        "p2l1": "0",
        "p2l2": "0",
        "p3e1": "0",
        "p3e2": "0",
        "p3l1": "0",
        "p3l2": "0",
        # Modulation matrix: pitch
        "f1e1": "0",
        "f1e2": "0",
        "f1l1": "0",
        "f1l2": "0",
        "f2e1": "0",
        "f2e2": "0",
        "f2l1": "0",
        "f2l2": "0",
        "f3e1": "0",
        "f3e2": "0",
        "f3l1": "0",
        "f3l2": "0",
        # Modulation matrix: pulse width
        "w1e1": "0",
        "w1e2": "0",
        "w1l1": "0",
        "w1l2": "0",
        # Modulation matrix: sub-oscillator
        "s3e1": "0",
        "s3e2": "0",
        "s3l1": "0",
        "s3l2": "0",
    },
    "nes": {
        # Channel 1 (square with sweep)
        "on1": "1",
        "crs1": "0",
        "vol1": "15",
        "envon1": "0",
        "envloop1": "0",
        "envlen1": "0",
        "dc1": "0",
        "sweep1": "0",
        "swamt1": "0",
        "swrate1": "0",
        # Channel 2 (square)
        "on2": "1",
        "crs2": "0",
        "vol2": "15",
        "envon2": "0",
        "envloop2": "0",
        "envlen2": "0",
        "dc2": "2",
        "sweep2": "0",
        "swamt2": "0",
        "swrate2": "0",
        # Channel 3 (triangle)
        "on3": "1",
        "crs3": "0",
        "vol3": "15",
        # Channel 4 (noise)
        "on4": "0",
        "vol4": "15",
        "envon4": "0",
        "envloop4": "0",
        "envlen4": "0",
        "nmode4": "0",
        "nfrqmode4": "0",
        "nfreq4": "0",
        "nq4": "1",
        "nswp4": "0",
        # Master
        "vol": "1",
        "vibr": "0",
    },
    "organic": {
        # num_osc is saved as XML attribute, not via saveSettings
        "num_osc": "8",
        "foldback": "0",
        "vol": "100",
        # Per-oscillator params (8 oscillators, harmonic default = index)
        "vol0": "100",
        "pan0": "0",
        "newharmonic0": "0",
        "newdetune0": "0",
        "wavetype0": "0",
        "vol1": "100",
        "pan1": "0",
        "newharmonic1": "1",
        "newdetune1": "0",
        "wavetype1": "0",
        "vol2": "100",
        "pan2": "0",
        "newharmonic2": "2",
        "newdetune2": "0",
        "wavetype2": "0",
        "vol3": "100",
        "pan3": "0",
        "newharmonic3": "3",
        "newdetune3": "0",
        "wavetype3": "0",
        "vol4": "100",
        "pan4": "0",
        "newharmonic4": "4",
        "newdetune4": "0",
        "wavetype4": "0",
        "vol5": "100",
        "pan5": "0",
        "newharmonic5": "5",
        "newdetune5": "0",
        "wavetype5": "0",
        "vol6": "100",
        "pan6": "0",
        "newharmonic6": "6",
        "newdetune6": "0",
        "wavetype6": "0",
        "vol7": "100",
        "pan7": "0",
        "newharmonic7": "7",
        "newdetune7": "0",
        "wavetype7": "0",
    },
    "freeboy": {
        # Channel 1 sweep
        "st": "4",
        "sd": "0",
        "srs": "4",
        # Channel 1
        "ch1wpd": "2",
        "ch1vol": "15",
        "ch1vsd": "0",
        "ch1ssl": "0",
        # Channel 2
        "ch2wpd": "2",
        "ch2vol": "15",
        "ch2vsd": "0",
        "ch2ssl": "0",
        # Channel 3
        "ch3vol": "3",
        # Channel 4
        "ch4vol": "15",
        "ch4vsd": "0",
        "ch4ssl": "0",
        "srw": "0",
        # Output routing
        "so1vol": "7",
        "so2vol": "7",
        "ch1so2": "1",
        "ch2so2": "1",
        "ch3so2": "1",
        "ch4so2": "0",
        "ch1so1": "1",
        "ch2so1": "1",
        "ch3so1": "1",
        "ch4so1": "0",
        # Equalizer
        "Treble": "-20",
        "Bass": "461",
        # Waveform (base64-encoded graph data)
        "sampleShape": "",
    },
    "opulenz": {
        # Operator 1
        "op1_a": "14",
        "op1_d": "14",
        "op1_s": "3",
        "op1_r": "10",
        "op1_lvl": "62",
        "op1_scale": "0",
        "op1_mul": "0",
        "feedback": "0",
        "op1_ksr": "0",
        "op1_perc": "0",
        "op1_trem": "1",
        "op1_vib": "0",
        "op1_waveform": "0",
        # Operator 2
        "op2_a": "1",
        "op2_d": "3",
        "op2_s": "14",
        "op2_r": "12",
        "op2_lvl": "63",
        "op2_scale": "0",
        "op2_mul": "1",
        "op2_ksr": "0",
        "op2_perc": "0",
        "op2_trem": "0",
        "op2_vib": "1",
        "op2_waveform": "0",
        # Global
        "fm": "1",
        "vib_depth": "0",
        "trem_depth": "0",
    },
    "sid": {
        # Voice 0 (Triangle waveform=1, FilterType::LowPass=2, ChipModel::MOS8580=1)
        "pulsewidth0": "2048",
        "attack0": "8",
        "decay0": "8",
        "sustain0": "15",
        "release0": "8",
        "coarse0": "0",
        "waveform0": "1",
        "sync0": "0",
        "ringmod0": "0",
        "filtered0": "0",
        "test0": "0",
        # Voice 1
        "pulsewidth1": "2048",
        "attack1": "8",
        "decay1": "8",
        "sustain1": "15",
        "release1": "8",
        "coarse1": "0",
        "waveform1": "1",
        "sync1": "0",
        "ringmod1": "0",
        "filtered1": "0",
        "test1": "0",
        # Voice 2
        "pulsewidth2": "2048",
        "attack2": "8",
        "decay2": "8",
        "sustain2": "15",
        "release2": "8",
        "coarse2": "0",
        "waveform2": "1",
        "sync2": "0",
        "ringmod2": "0",
        "filtered2": "0",
        "test2": "0",
        # Filter
        "filterFC": "1024",
        "filterResonance": "8",
        "filterMode": "2",
        # Misc
        "voice3Off": "0",
        "volume": "15",
        "chipModel": "1",
    },
    "bitinvader": {
        "version": "0.1",
        "sampleLength": "200",
        "sampleShape": "",
        "interpolation": "0",
        "normalize": "0",
    },
    "watsyn": {
        # Volumes
        "a1_vol": "100",
        "a2_vol": "100",
        "b1_vol": "100",
        "b2_vol": "100",
        # Panning
        "a1_pan": "0",
        "a2_pan": "0",
        "b1_pan": "0",
        "b2_pan": "0",
        # Frequency multipliers
        "a1_mult": "8",
        "a2_mult": "8",
        "b1_mult": "8",
        "b2_mult": "8",
        # Left detuning
        "a1_ltune": "0",
        "a2_ltune": "0",
        "b1_ltune": "0",
        "b2_ltune": "0",
        # Right detuning
        "a1_rtune": "0",
        "a2_rtune": "0",
        "b1_rtune": "0",
        "b2_rtune": "0",
        # Waveforms (base64-encoded graph data, sine by default)
        "a1_wave": "",
        "a2_wave": "",
        "b1_wave": "",
        "b2_wave": "",
        # Mix and envelope
        "abmix": "0",
        "envAmt": "0",
        "envAtt": "0",
        "envHold": "0",
        "envDec": "0",
        # Crosstalk and modulation
        "xtalk": "0",
        "amod": "0",
        "bmod": "0",
    },
    "vibed": {
        "version": "0.2",
        # String 0 (active by default)
        "active0": "1",
        "volume0": "100",
        "stiffness0": "0",
        "pick0": "0",
        "pickup0": "0.05",
        "octave0": "2",
        "length0": "1",
        "pan0": "0",
        "detune0": "0",
        "slap0": "0",
        "impulse0": "0",
        "graph0": "",
        # String 1
        "active1": "0",
        "volume1": "100",
        "stiffness1": "0",
        "pick1": "0",
        "pickup1": "0.05",
        "octave1": "2",
        "length1": "1",
        "pan1": "0",
        "detune1": "0",
        "slap1": "0",
        "impulse1": "0",
        "graph1": "",
        # String 2
        "active2": "0",
        "volume2": "100",
        "stiffness2": "0",
        "pick2": "0",
        "pickup2": "0.05",
        "octave2": "2",
        "length2": "1",
        "pan2": "0",
        "detune2": "0",
        "slap2": "0",
        "impulse2": "0",
        "graph2": "",
        # String 3
        "active3": "0",
        "volume3": "100",
        "stiffness3": "0",
        "pick3": "0",
        "pickup3": "0.05",
        "octave3": "2",
        "length3": "1",
        "pan3": "0",
        "detune3": "0",
        "slap3": "0",
        "impulse3": "0",
        "graph3": "",
        # String 4
        "active4": "0",
        "volume4": "100",
        "stiffness4": "0",
        "pick4": "0",
        "pickup4": "0.05",
        "octave4": "2",
        "length4": "1",
        "pan4": "0",
        "detune4": "0",
        "slap4": "0",
        "impulse4": "0",
        "graph4": "",
        # String 5
        "active5": "0",
        "volume5": "100",
        "stiffness5": "0",
        "pick5": "0",
        "pickup5": "0.05",
        "octave5": "2",
        "length5": "1",
        "pan5": "0",
        "detune5": "0",
        "slap5": "0",
        "impulse5": "0",
        "graph5": "",
        # String 6
        "active6": "0",
        "volume6": "100",
        "stiffness6": "0",
        "pick6": "0",
        "pickup6": "0.05",
        "octave6": "2",
        "length6": "1",
        "pan6": "0",
        "detune6": "0",
        "slap6": "0",
        "impulse6": "0",
        "graph6": "",
        # String 7
        "active7": "0",
        "volume7": "100",
        "stiffness7": "0",
        "pick7": "0",
        "pickup7": "0.05",
        "octave7": "2",
        "length7": "1",
        "pan7": "0",
        "detune7": "0",
        "slap7": "0",
        "impulse7": "0",
        "graph7": "",
        # String 8
        "active8": "0",
        "volume8": "100",
        "stiffness8": "0",
        "pick8": "0",
        "pickup8": "0.05",
        "octave8": "2",
        "length8": "1",
        "pan8": "0",
        "detune8": "0",
        "slap8": "0",
        "impulse8": "0",
        "graph8": "",
    },
    "sfxr": {
        "version": "1",
        # Envelope
        "att": "0",
        "hold": "0.3",
        "sus": "0",
        "dec": "0.4",
        # Frequency
        "startFreq": "0.3",
        "minFreq": "0",
        "slide": "0",
        "dSlide": "0",
        "vibDepth": "0",
        "vibSpeed": "0",
        # Change
        "changeAmt": "0",
        "changeSpeed": "0",
        # Square duty
        "sqrDuty": "0",
        "sqrSweep": "0",
        # Repeat
        "repeatSpeed": "0",
        # Phaser
        "phaserOffset": "0",
        "phaserSweep": "0",
        # Filters
        "lpFilCut": "1",
        "lpFilCutSweep": "0",
        "lpFilReso": "0",
        "hpFilCut": "0",
        "hpFilCutSweep": "0",
        # Waveform (0=Square, 1=Saw, 2=Sine, 3=Noise)
        "waveForm": "0",
    },
    "xpressive": {
        "version": "0.1",
        # Output expressions (default mathematical formulas)
        "O1": "sinew(integrate(f*(1+0.05sinew(12t))))*(2^(-(1.1+A2)*t)*(0.4+0.1(1+A3)+0.4sinew((2.5+2A1)t))^2)",
        "O2": "expw(integrate(f*atan(500t)*2/pi))*0.5+0.12",
        # Wave expressions and sample data (base64-encoded)
        "W1": "",
        "W1sample": "",
        "W2": "",
        "W2sample": "",
        "W3": "",
        "W3sample": "",
        # Wave smoothing
        "smoothW1": "0",
        "smoothW2": "0",
        "smoothW3": "0",
        # Wave interpolation
        "interpolateW1": "0",
        "interpolateW2": "0",
        "interpolateW3": "0",
        # Parameters
        "A1": "1",
        "A2": "1",
        "A3": "1",
        # Panning
        "PAN1": "1",
        "PAN2": "-1",
        # Release transition
        "RELTRANS": "50",
    },
    "slicert": {
        "version": "1",
        "src": "",
        "fadeOut": "10",
        "threshold": "0.6",
        "origBPM": "1",
        "syncEnable": "0",
    },
    "malletsstk": {
        # ModalBar controls
        "hardness": "64",
        "position": "64",
        "vib_gain": "0",
        "vib_freq": "0",
        "stick_mix": "0",
        # TubeBell controls
        "modulator": "64",
        "crossfade": "64",
        "lfo_speed": "64",
        "lfo_depth": "64",
        "adsr": "64",
        # BandedWG controls
        "pressure": "64",
        "velocity": "64",
        "strike": "1",
        # Shared controls
        "preset": "0",
        "spread": "0",
        "randomness": "0",
        "version": "1",
        "oldversion": "0",
    },
    "sf2player": {
        "src": "",
        "patch": "0",
        "bank": "0",
        "gain": "1",
        # Reverb (FluidSynth defaults)
        "reverbOn": "0",
        "reverbRoomSize": "0.2",
        "reverbDamping": "0",
        "reverbWidth": "0.5",
        "reverbLevel": "0.9",
        # Chorus (FluidSynth defaults)
        "chorusOn": "0",
        "chorusNum": "3",
        "chorusLevel": "2",
        "chorusSpeed": "0.3",
        "chorusDepth": "8",
    },
    "patman": {
        "src": "",
        "looped": "1",
        "tuned": "1",
    },
    "gigplayer": {
        "src": "",
        "patch": "0",
        "bank": "0",
        "gain": "1",
    },
}


# ============================================================
# C. Effect plugin defaults (keyed by lowercase internal name)
# ============================================================

EFFECT_PLUGIN_DEFAULTS: dict[str, dict[str, Any]] = {
    "reverbsc": {
        "color": "10000",
        "input_gain": "0",
        "output_gain": "0",
        "size": "0.89",
    },
    "delay": {
        "delay": "0.5",
        "delay_denominator": "4",
        "delay_numerator": "4",
        "delay_syncmode": "0",
        "feedback": "0",
        "lfo_amount": "0",
        "lfo_frequency": "2",
        "lfo_frequency_denominator": "4",
        "lfo_frequency_numerator": "4",
        "lfo_frequency_syncmode": "0",
        "output_gain": "0",
    },
    "amplifier": {
        "volume": "100",
        "pan": "0",
        "left": "100",
        "right": "100",
    },
    "bassbooster": {
        "freq": "100",
        "gain": "1",
        "ratio": "2",
    },
    "bitcrush": {
        "ingain": "0",
        "innoise": "0",
        "outgain": "0",
        "outclip": "0",
        "rate": "44100",
        "stereodiff": "0",
        "levels": "256",
        "rateon": "1",
        "depthon": "1",
    },
    "compressor": {
        "threshold": "-8",
        "ratio": "1.8",
        "attack": "10",
        "release": "100",
        "knee": "12",
        "hold": "0",
        "range": "-240",
        "rms": "1",
        "midside": "0",
        "peakmode": "0",
        "lookaheadLength": "0",
        "inBalance": "0",
        "outBalance": "0",
        "limiter": "0",
        "outGain": "0",
        "inGain": "0",
        "blend": "1",
        "stereoBalance": "0",
        "autoMakeup": "0",
        "audition": "0",
        "feedback": "0",
        "autoAttack": "0",
        "autoRelease": "0",
        "lookahead": "0",
        "tilt": "0",
        "tiltFreq": "150",
        "stereoLink": "1",
        "mix": "100",
    },
    "crossovereq": {
        "xover12": "125",
        "xover23": "1250",
        "xover34": "5000",
        "gain1": "0",
        "gain2": "0",
        "gain3": "0",
        "gain4": "0",
        "mute1": "1",
        "mute2": "1",
        "mute3": "1",
        "mute4": "1",
    },
    "dispersion": {
        "amount": "0",
        "freq": "200",
        "reso": "0.707",
        "feedback": "0",
        "dc": "0",
    },
    "dualfilter": {
        "enabled1": "1",
        "filter1": "0",
        "cut1": "7000",
        "res1": "0.5",
        "gain1": "100",
        "mix": "0",
        "enabled2": "1",
        "filter2": "0",
        "cut2": "7000",
        "res2": "0.5",
        "gain2": "100",
    },
    "dynamicsprocessor": {
        "inputGain": "1",
        "outputGain": "1",
        "attack": "10",
        "release": "100",
        "stereoMode": "0",
        "waveShape": (
            "CtejOwrXIzyPwnU8CtejPM3MzDyPwvU8KVwPPQrXIz3sUTg9zcxMPa5HYT2P"
            "wnU9uB6FPSlcjz2amZk9CtejPXsUrj3sUbg9XI/CPc3MzD09Ctc9rkfhPR+F6"
            "z2PwvU9AAAAPrgeBT5xPQo+KVwPPuF6FD6amRk+UrgePgrXIz7D9Sg+exQuPjM"
            "zMz7sUTg+pHA9PlyPQj4Urkc+zcxMPoXrUT49Clc+9ihcPq5HYT5mZmY+H4Vr"
            "PtejcD6PwnU+SOF6PgAAgD5cj4I+uB6FPhSuhz5xPYo+zcyMPilcjz6F65E+4"
            "XqUPj0Klz6amZk+9iicPlK4nj6uR6E+CtejPmZmpj7D9ag+H4WrPnsUrj7Xo7"
            "A+MzOzPo/CtT7sUbg+SOG6PqRwvT4AAMA+XI/CPrgexT4Ursc+cT3KPs3MzD4p"
            "XM8+hevRPuF61D49Ctc+mpnZPvYo3D5SuN4+rkfhPgrX4z5mZuY+w/XoPh+F6"
            "z57FO4+16PwPjMz8z6PwvU+7FH4Pkjh+j6kcP0+AAAAP65HAT9cjwI/CtcDP7"
            "geBT9mZgY/FK4HP8P1CD9xPQo/H4ULP83MDD97FA4/KVwPP9ejED+F6xE/MzM"
            "TP+F6FD+PwhU/PQoXP+xRGD+amRk/SOEaP/YoHD+kcB0/UrgePwAAID+uRyE/"
            "XI8iPwrXIz+4HiU/ZmYmPxSuJz/D9Sg/cT0qPx+FKz/NzCw/exQuPylcLz/Xo"
            "zA/hesxPzMzMz/hejQ/j8I1Pz0KNz/sUTg/mpk5P0jhOj/2KDw/pHA9P1K4Pj"
            "8AAEA/rkdBP1yPQj8K10M/uB5FP2ZmRj8Urkc/w/VIP3E9Sj8fhUs/zcxMP3s"
            "UTj8pXE8/16NQP4XrUT8zM1M/4XpUP4/CVT89Clc/7FFYP5qZWT9I4Vo/9ihc"
            "P6RwXT9SuF4/AABgP65HYT9cj2I/CtdjP7geZT9mZmY/FK5nP8P1aD9xPWo/H"
            "4VrP83MbD97FG4/KVxvP9ejcD+F63E/MzNzP+F6dD+PwnU/PQp3P+xReD+amX"
            "k/SOF6P/YofD+kcH0/Urh+PwAAgD8="
        ),
    },
    "eq": {
        "Inputgain": "0",
        "Outputgain": "0",
        "Lowshelfgain": "0",
        "Peak1gain": "0",
        "Peak2gain": "0",
        "Peak3gain": "0",
        "Peak4gain": "0",
        "HighShelfgain": "0",
        "HPres": "0.707",
        "LowShelfres": "0.707",
        "Peak1bw": "0.3",
        "Peak2bw": "0.3",
        "Peak3bw": "0.3",
        "Peak4bw": "0.3",
        "HighShelfres": "0.707",
        "LPres": "0.707",
        "HPfreq": "31",
        "LowShelffreq": "80",
        "Peak1freq": "120",
        "Peak2freq": "250",
        "Peak3freq": "2000",
        "Peak4freq": "4000",
        "Highshelffreq": "12000",
        "LPfreq": "18000",
        "HPactive": "0",
        "Lowshelfactive": "0",
        "Peak1active": "0",
        "Peak2active": "0",
        "Peak3active": "0",
        "Peak4active": "0",
        "Highshelfactive": "0",
        "LPactive": "0",
        "LP12": "0",
        "LP24": "0",
        "LP48": "0",
        "HP12": "0",
        "HP24": "0",
        "HP48": "0",
        "LP": "0",
        "HP": "0",
        "AnalyseIn": "1",
        "AnalyseOut": "1",
    },
    "flanger": {
        "DelayTimeSamples": "0.001",
        "LfoFrequency": "0.25",
        "LfoAmount": "0",
        "LfoPhase": "90",
        "Feedback": "0",
        "WhiteNoise": "0",
        "Invert": "0",
    },
    "granularpitchshifter": {
        "range": "0",
        "pitch": "1",
        "size": "10",
        "spray": "0.005",
        "jitter": "0",
        "twitch": "0",
        "pitchSpread": "0",
        "spraySpread": "0",
        "shape": "2",
        "fadeLength": "1",
        "feedback": "0",
        "minLatency": "0.01",
        "prefilter": "1",
        "density": "1",
        "glide": "0.01",
    },
    "lomm": {
        "depth": "0.4",
        "time": "1",
        "inVol": "0",
        "outVol": "8",
        "upward": "1",
        "downward": "1",
        "split1": "2500",
        "split2": "88.3",
        "split1Enabled": "1",
        "split2Enabled": "1",
        "band1Enabled": "1",
        "band2Enabled": "1",
        "band3Enabled": "1",
        "inHigh": "0",
        "inMid": "0",
        "inLow": "0",
        "outHigh": "4.6",
        "outMid": "0",
        "outLow": "4.6",
        "aThreshH": "-30.3",
        "aThreshM": "-25",
        "aThreshL": "-28.6",
        "aRatioH": "99.99",
        "aRatioM": "66.7",
        "aRatioL": "66.7",
        "bThreshH": "-35.6",
        "bThreshM": "-36.6",
        "bThreshL": "-35.6",
        "bRatioH": "4.17",
        "bRatioM": "4.17",
        "bRatioL": "4.17",
        "atkH": "13.5",
        "atkM": "22.4",
        "atkL": "47.8",
        "relH": "132",
        "relM": "282",
        "relL": "282",
        "rmsTime": "10",
        "knee": "6",
        "range": "36",
        "balance": "0",
        "depthScaling": "1",
        "stereoLink": "0",
        "autoTime": "0",
        "mix": "1",
        "feedback": "0",
        "midside": "0",
        "lookaheadEnable": "0",
        "lookahead": "0",
        "lowSideUpwardSuppress": "0",
    },
    "multitapecho": {
        "steps": "16",
        "steplength": "100",
        "drygain": "0",
        "swapinputs": "0",
        "stages": "1",
        "ampsteps": "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA==",
        "lpsteps": "AABAQAAAQEAAAEBAAABAQAAAQEAAAEBAAABAQAAAQEAAAEBAAABAQAAAQEAAAEBAAABAQAAAQEAAAEBAAABAQA==",
    },
    "peakcontrollereffect": {
        "base": "0.5",
        "amount": "1",
        "mute": "0",
        "attack": "0",
        "decay": "0",
        "abs": "1",
        "amountmult": "1",
        "treshold": "0",
    },
    "stereoenhancer": {
        "width": "0",
    },
    "stereomatrix": {
        "l-l": "1",
        "l-r": "0",
        "r-l": "0",
        "r-r": "1",
    },
    "waveshaper": {
        "inputGain": "1",
        "outputGain": "1",
        "clipInput": "0",
        "waveShape": (
            "CtejOwrXIzyPwnU8CtejPM3MzDyPwvU8KVwPPQrXIz3sUTg9zcxMPa5HYT2P"
            "wnU9uB6FPSlcjz2amZk9CtejPXsUrj3sUbg9XI/CPc3MzD09Ctc9rkfhPR+F6"
            "z2PwvU9AAAAPrgeBT5xPQo+KVwPPuF6FD6amRk+UrgePgrXIz7D9Sg+exQuPjM"
            "zMz7sUTg+pHA9PlyPQj4Urkc+zcxMPoXrUT49Clc+9ihcPq5HYT5mZmY+H4Vr"
            "PtejcD6PwnU+SOF6PgAAgD5cj4I+uB6FPhSuhz5xPYo+zcyMPilcjz6F65E+4"
            "XqUPj0Klz6amZk+9iicPlK4nj6uR6E+CtejPmZmpj7D9ag+H4WrPnsUrj7Xo7"
            "A+MzOzPo/CtT7sUbg+SOG6PqRwvT4AAMA+XI/CPrgexT4Ursc+cT3KPs3MzD4p"
            "XM8+hevRPuF61D49Ctc+mpnZPvYo3D5SuN4+rkfhPgrX4z5mZuY+w/XoPh+F6"
            "z57FO4+16PwPjMz8z6PwvU+7FH4Pkjh+j6kcP0+AAAAP65HAT9cjwI/CtcDP7"
            "geBT9mZgY/FK4HP8P1CD9xPQo/H4ULP83MDD97FA4/KVwPP9ejED+F6xE/MzM"
            "TP+F6FD+PwhU/PQoXP+xRGD+amRk/SOEaP/YoHD+kcB0/UrgePwAAID+uRyE/"
            "XI8iPwrXIz+4HiU/ZmYmPxSuJz/D9Sg/cT0qPx+FKz/NzCw/exQuPylcLz/Xo"
            "zA/hesxPzMzMz/hejQ/j8I1Pz0KNz/sUTg/mpk5P0jhOj/2KDw/pHA9P1K4Pj"
            "8AAEA/rkdBP1yPQj8K10M/uB5FP2ZmRj8Urkc/w/VIP3E9Sj8fhUs/zcxMP3s"
            "UTj8pXE8/16NQP4XrUT8zM1M/4XpUP4/CVT89Clc/7FFYP5qZWT9I4Vo/9ihc"
            "P6RwXT9SuF4/AABgP65HYT9cj2I/CtdjP7geZT9mZmY/FK5nP8P1aD9xPWo/H"
            "4VrP83MbD97FG4/KVxvP9ejcD+F63E/MzNzP+F6dD+PwnU/PQp3P+xReD+amX"
            "k/SOF6P/YofD+kcH0/Urh+PwAAgD8="
        ),
    },
    "spectrumanalyzer": {
        "Waterfall": "0",
        "Smooth": "0",
        "Stereo": "0",
        "PeakHold": "0",
        "LogX": "1",
        "LogY": "1",
        "RangeX": "0",
        "RangeY": "1",
        "BlockSize": "3",
        "WindowType": "1",
        "EnvelopeRes": "0.25",
        "SpectrumRes": "1.5",
        "PeakDecayFactor": "0.992",
        "AverageWeight": "0.15",
        "WaterfallHeight": "300",
        "WaterfallGamma": "0.3",
        "WindowOverlap": "2",
        "ZeroPadding": "2",
    },
    "vectorscope": {
        "Persistence": "0.5",
        "Logarithmic": "0",
        "HighQuality": "0",
    },
}


# ============================================================
# D. Helper functions
# ============================================================

def normalize_plugin_name(name: str) -> str:
    """Normalize a plugin name to the lowercase form used in LMMS save files."""
    return name.lower()


def get_instrument_defaults(plugin_name: str) -> dict:
    """Return deep copy of default params for an instrument plugin.

    Returns empty dict for unknown plugins (permissive fallback).
    """
    normalized = normalize_plugin_name(plugin_name)
    return copy.deepcopy(INSTRUMENT_PLUGIN_DEFAULTS.get(normalized, {}))


def get_effect_defaults(plugin_name: str) -> dict:
    """Return deep copy of default params for an effect plugin.

    Returns empty dict for unknown plugins (permissive fallback).
    """
    normalized = normalize_plugin_name(plugin_name)
    return copy.deepcopy(EFFECT_PLUGIN_DEFAULTS.get(normalized, {}))


def get_track_subsystem_defaults() -> dict[str, str]:
    """Return all track subsystem JSON defaults as serialized JSON strings.

    Keys match the instrument_track table columns:
      sound_shaping_json, arpeggio_json, chord_creator_json,
      midi_port_json, track_extra_json, instrumenttrack_extra_json
    """
    return {
        "sound_shaping_json": json.dumps(copy.deepcopy(SOUND_SHAPING)),
        "arpeggio_json": json.dumps(copy.deepcopy(ARPEGGIO)),
        "chord_creator_json": json.dumps(copy.deepcopy(CHORD_CREATOR)),
        "midi_port_json": json.dumps(copy.deepcopy(MIDI_PORT)),
        "track_extra_json": json.dumps(copy.deepcopy(TRACK_EXTRA)),
        "instrumenttrack_extra_json": json.dumps(copy.deepcopy(INSTRUMENTTRACK_EXTRA)),
    }


def merge_params(defaults: dict, overrides: dict) -> dict:
    """Shallow-merge user overrides on top of plugin defaults.

    Top-level keys from overrides replace defaults. Nested dicts
    (like envelope targets) are replaced entirely if overridden.
    """
    result = copy.deepcopy(defaults)
    result.update(overrides)
    return result
