# Translations

FluxDM uses Qt's translation tools. Each language is one `fluxdm_<code>.ts` file here; the build
compiles it into the app, and Settings > General > Language lists it automatically.

## Add a language

1. Add `i18n/fluxdm_<code>.ts` (e.g. `fluxdm_de.ts`) to `TS_FILES` in `qt_add_translations` in
   `CMakeLists.txt`.
2. Run `scripts\build.ps1 -UpdateTranslations`: lupdate collects every `qsTr()`/`tr()` string into the file.
3. Translate it with Qt Linguist (`C:\Qt\6.8.3\msvc2022_64\bin\linguist.exe`) or any `.ts` editor.
   Keep placeholders like `%1` and `%n`. One-letter day names carry a note saying which day they are.
4. Build again. Strings left untranslated show in English.

## Update after code changes

`scripts\build.ps1 -UpdateTranslations` adds new strings (marked unfinished) and drops removed ones.

## Status

| Language | State |
|---|---|
| Spanish (`es`) | Complete, machine-translated: needs review by a native speaker. |
