# Effect Plugin Defaults Extraction

User asked to extract default parameter values for 19 LMMS native effect plugins from C++ source code. The goal is to produce Python dictionary literals for `lmms_defaults.py`.

Read all `*Controls.cpp` and `*Controls.h` files for each plugin, extracted constructor default values and save names from `saveSettings()` methods.

Produced complete Python dict entries for all 19 plugins.
